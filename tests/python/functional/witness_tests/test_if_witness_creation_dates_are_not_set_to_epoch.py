from __future__ import annotations

from typing import TYPE_CHECKING

from hive_local_tools import run_for

if TYPE_CHECKING:
    import test_tools as tt


@run_for("mainnet_5m")
def test_if_witness_creation_dates_are_not_set_to_epoch(
    node: tt.InitNode | tt.RemoteNode,
) -> None:
    def get_witnesses(first_witness_index: int, amount: int = 1000) -> list[dict | None]:
        witness_ids = list(range(first_witness_index, first_witness_index + amount))
        return node.api.condenser.get_witnesses(witness_ids)

    all_witnesses: list[dict] = []
    # Zeroth witness it is an 'initminer' (id: 0) artificially created with date "1970-01-01T00:00:00". Loop starts from first real witness.
    witness_id = 1
    while True:
        witnesses = get_witnesses(witness_id)
        if not all(witnesses):
            all_witnesses.extend(witnesses[: witnesses.index(None)])
            break

        all_witnesses.extend(witnesses)

        witness_id += 1000

    for witness in all_witnesses:
        assert witness["created"] != "1970-01-01T00:00:00"
