/*
 * mod-llm-chatter - AH intent-system action commands.
 *
 * `.llmc ahlist <botName> <itemEntry> <buyoutCopper>` and
 * `.llmc ahbuy <botName> <auctionId>` are the execution side of
 * the gear/gold intent system (see tools/chatter_intent.py) —
 * a bot lists an item it's outgrown, another bot's gold-need
 * check finds and buys it. This is the ONLY part of the intent
 * system that touches live game state (money, items); the
 * detection side is pure DB reads.
 *
 * SOAP-reachable (see docs — SOAP is the general "intent action
 * execution" channel this session settled on), GM-level only.
 * Routed through the existing `.llmc` root command dispatch in
 * LLMChatterCommand.cpp rather than a second top-level command
 * registration — ChatCommandTable only allows one "llmc" entry,
 * and that root command already does its own manual string
 * dispatch instead of nested-table dispatch, so this matches
 * the established pattern instead of fighting it.
 *
 * DELIBERATE SAFETY CHOICE — online-only targets, no offline
 * fallback: this session found (the hard way, twice) that
 * editing a live-in-memory character's row directly via SQL
 * gets silently clobbered by the server's own periodic
 * autosave, because the in-memory Player object never learns
 * about the out-of-band edit. `.character level` and
 * `.teleport name` are safe for offline targets because the
 * core itself has dedicated offline-safe paths for those two
 * specific operations. Money and inventory don't have an
 * equivalent well-trodden offline path available here, and
 * getting that wrong risks real item/gold duplication or loss,
 * not just a cosmetic revert. The narrative cast is essentially
 * always online anyway (mod-playerbots keeps random bots logged
 * in regardless of real-player presence — see STATE.md), so
 * requiring an online target costs nothing in practice. Both
 * commands operate through the live Player object exactly the
 * way `.character level` does, and error cleanly on an offline
 * target instead of attempting anything against its DB row.
 *
 * ===========================================================
 * All API usage below verified 2026-09-14 against the real
 * source (~/azerothcore-playerbots-build on oci-arm1) — the
 * concrete item-create/auction-list sequence was cross-checked
 * against AuctionHouseHandler.cpp's own real player auction-
 * create packet handler, not just individual signatures. Two
 * real bugs were caught and fixed in this pass:
 *   - GenerateAuctionID() lives on sObjectMgr, not sAuctionMgr.
 *   - SaveInventoryAndGoldToDB(trans) was missing on both the
 *     lister and buyer — MoveItemFromInventory/ModifyMoney only
 *     update the live in-memory Player object, this persists it.
 * Also added an equipped-item guard in FindListableItem() that
 * wasn't in the original draft (see its own comment below).
 * ===========================================================
 */

#include "LLMChatterAuction.h"

#include "AuctionHouseMgr.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "LLMChatterShared.h"
// Confirmed real path is Mails/Mail.h, not Mail.h directly —
// works anyway because this codebase's CMake adds every game/
// subdirectory to the include search path (same reason
// "AuctionHouseMgr.h" and "Player.h" already worked bare).
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "WorldSession.h"

#include <ctime>
#include <sstream>
#include <string>

namespace
{
// Longest available auction duration — bots don't care about
// deposit-vs-duration tradeoffs the way a real player might.
constexpr time_t kAuctionDurationSeconds = 2 * 24 * 60 * 60; // 48h

// Internal GM-level gate. The `.llmc` root command is
// registered SEC_PLAYER (it's a player-facing addon bridge —
// see LLMChatterCommand.cpp) so these two admin-only
// subcommands enforce their own floor here, same style as
// that file's own IsKnownBotForPlayer() self-policing rather
// than relying on the table-level security flag.
bool RequireGMLevel(ChatHandler* handler)
{
    if (!handler || !handler->GetSession())
    {
        // SOAP/console sessions may not carry a normal
        // WorldSession the same way an in-game player does
        // on every core — if there's no session object at
        // all, don't block; the SOAP account's own security
        // level already gated getting this far. Flagged as
        // an UNVERIFIED assumption — if SOAP sessions in this
        // fork *do* carry a normal WorldSession (likely, per
        // this session's own successful `.character level`
        // testing via SOAP), this branch is dead code and the
        // check below is the one that actually applies.
        return true;
    }

    if (handler->GetSession()->GetSecurity() < SEC_GAMEMASTER)
    {
        handler->PSendSysMessage(
            "ahlist/ahbuy require GM level.");
        return false;
    }

    return true;
}

bool ParseUInt32(std::string const& token, uint32& out)
{
    if (token.empty())
        return false;

    for (char ch : token)
    {
        if (!std::isdigit(static_cast<unsigned char>(ch)))
            return false;
    }

    try
    {
        out = static_cast<uint32>(std::stoul(token));
    }
    catch (...)
    {
        return false;
    }

    return true;
}

// Verified 2026-09-14 against the real source
// (PlayerStorage.cpp): Player::GetItemByEntry checks inventory
// bags, keyring, and equipped bags before falling through to
// equipped-slot items last — so if a bag copy exists it's
// returned first, but if the ONLY copy is currently equipped,
// GetItemByEntry would hand that back. For "list gear I've
// outgrown" we want bag items only — never auto-unequip and
// sell what a bot is currently wearing — so explicitly reject
// an equipped result rather than trusting the fallthrough.
Item* FindListableItem(Player* bot, uint32 itemEntry)
{
    Item* item = bot->GetItemByEntry(itemEntry);
    if (!item)
        return nullptr;

    if (item->GetBagSlot() == INVENTORY_SLOT_BAG_0
        && item->GetSlot() >= EQUIPMENT_SLOT_START
        && item->GetSlot() < EQUIPMENT_SLOT_END)
    {
        // Only match was equipped — refuse rather than sell
        // gear the bot is currently wearing.
        return nullptr;
    }

    return item;
}
} // namespace

bool HandleAhListCommand(
    ChatHandler* handler, std::string const& args)
{
    if (!RequireGMLevel(handler))
        return true;

    std::istringstream iss(args);
    std::string botName;
    std::string itemToken;
    std::string buyoutToken;

    if (!(iss >> botName >> itemToken >> buyoutToken))
    {
        handler->PSendSysMessage(
            "Usage: .llmc ahlist <botName> <itemEntry> "
            "<buyoutCopper>");
        return true;
    }

    uint32 itemEntry = 0;
    uint32 buyoutCopper = 0;
    if (!ParseUInt32(itemToken, itemEntry)
        || !ParseUInt32(buyoutToken, buyoutCopper)
        || itemEntry == 0
        || buyoutCopper == 0)
    {
        handler->PSendSysMessage(
            "itemEntry and buyoutCopper must be "
            "positive numbers.");
        return true;
    }

    // Online-only by deliberate design — see file header.
    // Confirmed real (ObjectAccessor.h), and confirmed as the
    // actual call convention used elsewhere in this codebase
    // (e.g. Guild.cpp's own invite-by-name handler).
    Player* bot = ObjectAccessor::FindPlayerByName(botName);
    if (!bot || !bot->IsInWorld())
    {
        handler->PSendSysMessage(
            "Bot '{}' is not online — ahlist only "
            "supports online targets (see file header "
            "for why).",
            botName.c_str());
        return true;
    }

    Item* item = FindListableItem(bot, itemEntry);
    if (!item)
    {
        handler->PSendSysMessage(
            "{} doesn't have item {} in their bags.",
            bot->GetName().c_str(), itemEntry);
        return true;
    }

    // Confirmed real: sAuctionHouseStore + .LookupEntry() (both
    // DBCStores.h and the widely-used DBCStorage<T> pattern
    // elsewhere in this codebase, e.g. sItemStore.LookupEntry).
    AuctionHouseEntry const* ahEntry =
        sAuctionHouseStore.LookupEntry(
            static_cast<uint32>(AuctionHouseId::Neutral));
    if (!ahEntry)
    {
        handler->PSendSysMessage(
            "Could not look up the neutral auction "
            "house entry (AuctionHouse.dbc).");
        return true;
    }

    AuctionHouseObject* auctionHouse =
        sAuctionMgr->GetAuctionsMapByHouseId(
            AuctionHouseId::Neutral);
    if (!auctionHouse)
    {
        handler->PSendSysMessage(
            "Could not resolve the neutral auction "
            "house map.");
        return true;
    }

    CharacterDatabaseTransaction trans =
        CharacterDatabase.BeginTransaction();

    // Move the item out of the bot's bags and into AH holding —
    // this exact sequence (MoveItemFromInventory, then
    // DeleteFromInventoryDB + SaveToDB in the same transaction)
    // confirmed by reading AuctionHouseHandler.cpp's own real
    // player auction-create handler directly.
    bot->MoveItemFromInventory(
        item->GetBagSlot(), item->GetSlot(), true);
    item->DeleteFromInventoryDB(trans);
    item->SaveToDB(trans);

    // Confirmed real: void AddAItem(Item* it) — AuctionHouseMgr.h.
    sAuctionMgr->AddAItem(item);

    // Verified 2026-09-14: GenerateAuctionID lives on sObjectMgr,
    // not sAuctionMgr (confirmed in ObjectMgr.h/.cpp, and matches
    // real usage in AuctionHouseHandler.cpp's own auction-create
    // handler — mirrored below).
    auto* entry = new AuctionEntry();
    entry->Id = sObjectMgr->GenerateAuctionID();
    entry->houseId = AuctionHouseId::Neutral;
    entry->item_guid = item->GetGUID();
    entry->item_template = itemEntry;
    entry->itemCount = item->GetCount();
    entry->owner = bot->GetGUID();
    entry->startbid = buyoutCopper;
    entry->bid = 0;
    entry->buyout = buyoutCopper;
    entry->expire_time =
        time(nullptr) + kAuctionDurationSeconds;
    entry->bidder = ObjectGuid::Empty;
    // Deliberate simplification, not an oversight: real-money
    // AH deposits exist to discourage spam-listing from real
    // players. For bot-internal economy simulation that
    // concern doesn't apply, so this skips
    // AuctionHouseMgr's deposit-calculation call entirely
    // rather than guessing its signature.
    entry->deposit = 0;
    entry->auctionHouseEntry = ahEntry;

    auctionHouse->AddAuction(entry); // confirmed real
    entry->SaveToDB(trans);          // confirmed real
    // Confirmed required by mirroring the real handler
    // (AuctionHouseHandler.cpp) — MoveItemFromInventory only
    // updates the live in-memory Player object; this persists
    // that inventory-slot change (and any gold delta) into the
    // same transaction.
    bot->SaveInventoryAndGoldToDB(trans);

    CharacterDatabase.CommitTransaction(trans);

    handler->PSendSysMessage(
        "{} listed {} (entry {}) for {} copper buyout — "
        "auction {}.",
        bot->GetName().c_str(), item->GetTemplate()->Name1.c_str(),
        itemEntry, buyoutCopper, entry->Id);
    return true;
}

bool HandleAhBuyCommand(
    ChatHandler* handler, std::string const& args)
{
    if (!RequireGMLevel(handler))
        return true;

    std::istringstream iss(args);
    std::string botName;
    std::string auctionToken;

    if (!(iss >> botName >> auctionToken))
    {
        handler->PSendSysMessage(
            "Usage: .llmc ahbuy <botName> <auctionId>");
        return true;
    }

    uint32 auctionId = 0;
    if (!ParseUInt32(auctionToken, auctionId)
        || auctionId == 0)
    {
        handler->PSendSysMessage(
            "auctionId must be a positive number.");
        return true;
    }

    // Online-only by deliberate design — see file header.
    Player* buyer = ObjectAccessor::FindPlayerByName(botName);
    if (!buyer || !buyer->IsInWorld())
    {
        handler->PSendSysMessage(
            "Bot '{}' is not online — ahbuy only "
            "supports online targets (see file header "
            "for why).",
            botName.c_str());
        return true;
    }

    AuctionHouseObject* auctionHouse =
        sAuctionMgr->GetAuctionsMapByHouseId(
            AuctionHouseId::Neutral);
    if (!auctionHouse)
    {
        handler->PSendSysMessage(
            "Could not resolve the neutral auction "
            "house map.");
        return true;
    }

    AuctionEntry* entry =
        auctionHouse->GetAuction(auctionId); // confirmed real
    if (!entry)
    {
        handler->PSendSysMessage(
            "No active auction with id {}.", auctionId);
        return true;
    }

    if (entry->owner == buyer->GetGUID())
    {
        handler->PSendSysMessage(
            "{} can't buy their own auction.",
            buyer->GetName().c_str());
        return true;
    }

    // Confirmed real: uint32 GetMoney() const, bool
    // ModifyMoney(int32 amount, bool sendError = true) —
    // Player.h, matching exactly.
    if (buyer->GetMoney() < entry->buyout)
    {
        handler->PSendSysMessage(
            "{} doesn't have enough gold ({} copper "
            "needed).",
            buyer->GetName().c_str(), entry->buyout);
        return true;
    }

    Item* item = sAuctionMgr->GetAItem(entry->item_guid);
    if (!item)
    {
        handler->PSendSysMessage(
            "Auction {}'s item could not be loaded — "
            "database may be inconsistent, not "
            "completing the purchase.",
            auctionId);
        return true;
    }

    CharacterDatabaseTransaction trans =
        CharacterDatabase.BeginTransaction();

    buyer->ModifyMoney(
        -static_cast<int32>(entry->buyout));

    // Confirmed real: InventoryResult CanStoreItem(uint8 bag,
    // uint8 slot, ItemPosCountVec& dest, Item* pItem, bool swap
    // = false) const, and Item* StoreItem(ItemPosCountVec const&
    // pos, Item* pItem, bool update) — both Player.h, matching
    // exactly (distinct from StoreNewItem, which manufactures a
    // brand-new Item from a template — not what we want here,
    // since the Item object already exists from the AH holding
    // map).
    ItemPosCountVec dest;
    InventoryResult msg = buyer->CanStoreItem(
        NULL_BAG, NULL_SLOT, dest, item, false);
    bool mailedInstead = false;
    if (msg == EQUIP_ERR_OK)
    {
        buyer->StoreItem(dest, item, true);
    }
    else
    {
        // Bags full — standard AH fallback behavior, mail
        // it instead rather than failing the purchase.
        mailedInstead = true;
        MailDraft(
            "Auction won",
            "Your bags were full — item mailed instead.")
            .AddItem(item)
            .SendMailTo(
                trans,
                buyer,
                MailSender(
                    MAIL_AUCTION, entry->Id),
                MAIL_CHECK_MASK_COPIED);
    }

    // Confirmed real 2026-09-14: RemoveAItem(ObjectGuid itemGuid,
    // bool deleteFromDB = false, CharacterDatabaseTransaction*
    // trans = nullptr) — AuctionHouseMgr.h. Defaults match this
    // single-arg call; the item's DB row is handled via the
    // buyer's own StoreItem/mail path below instead.
    sAuctionMgr->RemoveAItem(entry->item_guid);
    entry->DeleteFromDB(trans);                 // confirmed real
    auctionHouse->RemoveAuction(entry);         // confirmed real
    // Same reasoning as ahlist — persist the buyer's inventory/
    // gold change into this transaction rather than relying on
    // the next periodic autosave.
    buyer->SaveInventoryAndGoldToDB(trans);

    // Mail proceeds to the seller regardless of whether
    // they're online — MailDraft delivery doesn't need a
    // live session, which sidesteps the online-only
    // restriction that applies to the buyer/seller's own
    // money and inventory above.
    MailDraft(
        "Auction sold",
        "Your auction sold for " + std::to_string(entry->buyout)
            + " copper.")
        .AddMoney(entry->buyout)
        .SendMailTo(
            trans,
            // MailReceiver's ObjectGuid::LowType constructor
            // takes the raw uint32 counter, not a full
            // ObjectGuid — .GetCounter() extracts it. Caught by
            // the actual compiler on this exact line (ambiguous
            // conversion) — this is the one place my source
            // verification pass didn't catch a real bug before
            // the build did; every other fix in this file was
            // caught by reading, this one only by compiling.
            MailReceiver(entry->owner.GetCounter()),
            MailSender(MAIL_AUCTION, entry->Id),
            MAIL_CHECK_MASK_COPIED);

    CharacterDatabase.CommitTransaction(trans);

    handler->PSendSysMessage(
        "{} bought auction {} for {} copper{}.",
        buyer->GetName().c_str(), auctionId, entry->buyout,
        mailedInstead ? " (mailed — bags were full)" : "");

    delete entry;
    return true;
}
