from hive_local_tools import run_for


@run_for("testnet", "mainnet_5m", "live_mainnet")
def test_get_market_history_buckets(node):
    node.api.market_history.get_market_history_buckets()