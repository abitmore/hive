import test_tools as tt

from ..local_tools import run_for

ACCOUNT_NAME = 'gtg'


@run_for('testnet', 'mainnet_5m', 'mainnet_64m')
def test_get_key_references(prepared_node, should_prepare):
    if should_prepare:
        wallet = tt.Wallet(attach_to=prepared_node)
        wallet.api.create_account('initminer', ACCOUNT_NAME, '{}')
    key = prepared_node.api.condenser.get_accounts([ACCOUNT_NAME])[0]['active']['key_auths'][0][0]
    result_account = prepared_node.api.account_by_key.get_key_references(keys=[key])['accounts'][0][0]

    assert ACCOUNT_NAME == result_account