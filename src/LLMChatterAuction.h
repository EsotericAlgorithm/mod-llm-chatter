/*
 * mod-llm-chatter - AH intent-system action commands
 * (`.llmc ahlist` / `.llmc ahbuy`). See LLMChatterAuction.cpp
 * for the "why" — this is intentionally its own file per the
 * module's own separation-of-concerns convention (see
 * docs/mod-llm-chatter-architecture.md), routed through the
 * existing `.llmc` root command in LLMChatterCommand.cpp
 * rather than registering a second top-level command.
 */

#ifndef LLM_CHATTER_AUCTION_H
#define LLM_CHATTER_AUCTION_H

#include <string>

class ChatHandler;

bool HandleAhListCommand(ChatHandler* handler, std::string const& args);
bool HandleAhBuyCommand(ChatHandler* handler, std::string const& args);

#endif
