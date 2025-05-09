from __future__ import annotations

import test_tools as tt

from .utilities import check_recurrent_transfer, check_recurrent_transfer_data, create_accounts


def test_recurrent_transfer(wallet: tt.OldWallet) -> None:
    create_accounts(wallet, "initminer", ["alice", "bob"])

    wallet.api.transfer_to_vesting("initminer", "alice", tt.Asset.Test(100))

    wallet.api.transfer_to_vesting("initminer", "bob", tt.Asset.Test(100))

    wallet.api.transfer("initminer", "alice", tt.Asset.Test(500), "banana")

    response = wallet.api.recurrent_transfer("alice", "bob", tt.Asset.Test(20), "banana-cherry", 24, 3)

    _value = check_recurrent_transfer_data(response)

    check_recurrent_transfer(_value, "alice", "bob", tt.Asset.Test(20), "banana-cherry", 24, "executions", 3, 0)

    _result = wallet.api.find_recurrent_transfers("alice")
    assert len(_result) == 1
    check_recurrent_transfer(
        _result[0], "alice", "bob", tt.Asset.Test(20), "banana-cherry", 24, "remaining_executions", 2, 0
    )

    response = wallet.api.recurrent_transfer("bob", "alice", tt.Asset.Test(0.9), "banana-lime", 25, 2)

    _value = check_recurrent_transfer_data(response)
    check_recurrent_transfer(_value, "bob", "alice", tt.Asset.Test(0.9), "banana-lime", 25, "executions", 2, 0)

    _result = wallet.api.find_recurrent_transfers("bob")
    assert len(_result) == 1
    check_recurrent_transfer(
        _result[0], "bob", "alice", tt.Asset.Test(0.9), "banana-lime", 25, "remaining_executions", 1, 0
    )

    response = wallet.api.recurrent_transfer("bob", "initminer", tt.Asset.Test(0.8), "banana-lemon", 26, 22)

    _value = check_recurrent_transfer_data(response)
    check_recurrent_transfer(_value, "bob", "initminer", tt.Asset.Test(0.8), "banana-lemon", 26, "executions", 22, 0)

    _result = wallet.api.find_recurrent_transfers("bob")
    assert len(_result) == 2
    check_recurrent_transfer(
        _result[0], "bob", "alice", tt.Asset.Test(0.9), "banana-lime", 25, "remaining_executions", 1, 0
    )
    check_recurrent_transfer(
        _result[1], "bob", "initminer", tt.Asset.Test(0.8), "banana-lemon", 26, "remaining_executions", 21, 0
    )
