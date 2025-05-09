from __future__ import annotations

import test_tools as tt

from .utilities import check_keys


def test_account_creation_in_different_ways(node: tt.InitNode, wallet: tt.OldWallet) -> None:
    old_accounts_number = len(wallet.api.list_accounts("a", 100))

    tt.logger.info("Waiting...")
    node.wait_number_of_blocks(18)

    wallet.api.claim_account_creation("initminer", tt.Asset.Test(0))

    wallet.api.claim_account_creation_nonblocking("initminer", tt.Asset.Test(0))

    key = "STM8grZpsMPnH7sxbMVZHWEu1D26F3GwLW1fYnZEuwzT4Rtd57AER"

    response = wallet.api.create_account_with_keys("initminer", "alice1", "{}", key, key, key, key)

    _operations = response["operations"]
    check_keys(_operations[0][1], key, key, key, key)

    response = wallet.api.get_account("alice1")

    key = "STM8grZpsMPnH7sxbMVZHWEu1D26F3GwLW1fYnZEuwzT4Rtd57AER"

    wallet.api.create_funded_account_with_keys(
        "initminer", "alice2", tt.Asset.Test(2), "banana", "{}", key, key, key, key
    )

    assert old_accounts_number + 2 == len(wallet.api.list_accounts("a", 100))


def test_account_creation_with_exception(wallet: tt.OldWallet) -> None:
    try:
        wallet.api.create_account_delegated("initminer", tt.Asset.Test(3), tt.Asset.Vest(6.123456), "alicex", "{}")
    except Exception as e:
        message = str(e)
        found = message.find("Account creation with delegation is deprecated as of Hardfork 20")
        assert found != -1

    try:
        key = "STM8grZpsMPnH7sxbMVZHWEu1D26F3GwLW1fYnZEuwzT4Rtd57AER"
        wallet.api.create_account_with_keys_delegated(
            "initminer", tt.Asset.Test(4), tt.Asset.Vest(6.123456), "alicey", "{}", key, key, key, key
        )
    except Exception as e:
        message = str(e)
        found = message.find("Account creation with delegation is deprecated as of Hardfork 20")
        assert found != -1
