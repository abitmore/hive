from __future__ import annotations

from beembase.operations import Update_proposal_votes

import hive_utils
import test_tools as tt


def vote_proposals(node, accounts, wif):
    tt.logger.info("Voting proposals...")

    idx = 0
    for idx, account in enumerate(accounts):
        proposal_set = [idx]
        tt.logger.info(f"Account {account['name']} voted for proposals: {','.join(str(x) for x in proposal_set)}")
        op = Update_proposal_votes(**{"voter": account["name"], "proposal_ids": proposal_set, "approve": True})
        node.finalizeOp(op, account["name"], "active")

    if wif is not None:
        hive_utils.debug_generate_blocks(node.rpc.url, wif, 5)
    else:
        hive_utils.common.wait_n_blocks(node.rpc.url, 5)
