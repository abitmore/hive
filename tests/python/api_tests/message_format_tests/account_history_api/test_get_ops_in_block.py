from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from beekeepy.exceptions import ErrorInResponseError

from hive_local_tools import run_for

if TYPE_CHECKING:
    import test_tools as tt


@pytest.mark.parametrize(
    ("block_num", "virtual_operation", "include_reversible"),
    [
        # Valid block num
        (1, True, True),
        ("1", True, True),
        (1.1, True, True),
        (True, True, True),  # block number given as bool is converted to number
        (None, True, True),  # block number given as None is converted to 0
        (0, True, True),  # returns an empty response, blocks are numbered from 1
        # Valid virtual operations
        # (1, True, True),  # tested above; virtual operations given as bool is converted to number (True:1, False:0)
        (1, False, True),
        (1, None, True),  # None is converted to 0
        (1, 0, True),  # virtual_operation as number is converted to bool
        (1, 1, True),
        (1, 2, True),
        # Valid include reversible
        # (1, True, True), # tested above
        (1, True, False),
        (1, True, "true"),
        (1, True, "false"),
        (1, True, 0),
        (1, True, 1),
    ],
)
@run_for("testnet", "mainnet_5m", "live_mainnet", enable_plugins=["account_history_api"])
def test_get_ops_in_block_with_correct_values(
    node: tt.InitNode | tt.RemoteNode,
    block_num: bool | float | None,
    virtual_operation: bool | int | None,
    include_reversible: bool | int | str,
) -> None:
    node.api.account_history.get_ops_in_block(
        block_num=block_num,
        only_virtual=virtual_operation,
        include_reversible=include_reversible,
    )


@pytest.mark.parametrize(
    ("block_num", "virtual_operation", "include_reversible"),
    [
        # Invalid block number
        ("incorrect_string_argument", True, True),
        ("", True, True),
        ([1], True, True),
        ({}, True, True),
        # Invalid virtual operation
        (1, "incorrect_string_argument", True),
        (1, [True], True),
        (1, {}, True),
        (1, "1", True),
        # Invalid include reversible
        (1, True, ""),
        (1, True, "TRUE"),
        (1, True, "FALSE"),
        (1, True, []),
        (1, True, {}),
    ],
)
@run_for("testnet", "mainnet_5m", "live_mainnet", enable_plugins=["account_history_api"])
def test_get_ops_in_block_with_incorrect_type_of_arguments(
    node: tt.InitNode | tt.RemoteNode,
    block_num: dict | int | list | str,
    virtual_operation: bool | dict | list | str,
    include_reversible: bool | dict | list | str,
) -> None:
    with pytest.raises(ErrorInResponseError):
        node.api.account_history.get_ops_in_block(
            block_num=block_num,
            only_virtual=virtual_operation,
            include_reversible=include_reversible,
        )
