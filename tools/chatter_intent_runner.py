#!/usr/bin/env python3
"""Standalone periodic runner for chatter_intent.run_intent_checks().

chatter_intent.py's own docstring recommends running it "as a
standalone periodic job... intentionally NOT imported by
llm_chatter_bridge.py yet" — this is that job. Deployed as its own
compose service (chatter-intent), not threaded into the bridge's
ThreadPoolExecutor loop, so a bug here can't touch the working chat
pipeline. Pure DB reads/writes against acore_characters +
acore_world.item_template — no LLM calls, no game-session
requirement, so it runs fine even with nobody online.

    python chatter_intent_runner.py --config /config/mod_llm_chatter.conf
"""

import argparse
import logging
import time

from chatter_intent import run_intent_checks
from chatter_shared import get_db_connection, parse_config

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s %(levelname)s [intent] %(message)s',
)
logger = logging.getLogger(__name__)

DEFAULT_INTERVAL_SECONDS = 300


def main() -> None:
    parser = argparse.ArgumentParser(
        description='Periodic gear/gold intent-check runner'
    )
    parser.add_argument(
        '--config', required=True,
        help='Path to mod_llm_chatter.conf',
    )
    args = parser.parse_args()

    while True:
        config = parse_config(args.config)

        if config.get('LLMChatter.Enable', '0') != '1':
            time.sleep(60)
            continue

        interval = DEFAULT_INTERVAL_SECONDS
        try:
            interval = int(config.get(
                'LLMChatter.Intent.CheckIntervalSeconds',
                DEFAULT_INTERVAL_SECONDS,
            ))
        except (TypeError, ValueError):
            pass

        try:
            db = get_db_connection(config)
            try:
                n = run_intent_checks(db, config)
                logger.info(
                    "checked %d cast bots for gear/gold "
                    "needs", n,
                )
            finally:
                db.close()
        except Exception:
            logger.error(
                "intent check pass failed", exc_info=True,
            )

        time.sleep(max(30, interval))


if __name__ == '__main__':
    main()
