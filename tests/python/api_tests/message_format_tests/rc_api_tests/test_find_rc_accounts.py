from __future__ import annotations

import test_tools as tt
from hive_local_tools import run_for


# Resource credits (RC) were introduced after block with number 5000000, that's why this test is performed only on
# testnet and current mainnet.
@run_for("testnet", "live_mainnet")
def test_find_rc_accounts(node: tt.InitNode | tt.RemoteNode, should_prepare: bool) -> None:
    if should_prepare:
        wallet = tt.Wallet(attach_to=node)
        wallet.create_account("gtg", hives=tt.Asset.Test(100), vests=tt.Asset.Test(100))
    accounts = node.api.rc.find_rc_accounts(accounts=["gtg"]).rc_accounts
    assert len(accounts) != 0
