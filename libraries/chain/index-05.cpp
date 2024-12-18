#include <hive/chain/hive_objects.hpp>

#include <hive/chain/index.hpp>

#include <hive/chain/util/type_registrar_definition.hpp>

namespace hive { namespace chain {

void initialize_core_indexes_05( database& db )
{
  HIVE_ADD_CORE_INDEX(db, feed_history_index);
  HIVE_ADD_CORE_INDEX(db, convert_request_index);
  HIVE_ADD_CORE_INDEX(db, collateralized_convert_request_index);
}

} }

HIVE_DEFINE_TYPE_REGISTRAR_REGISTER_TYPE(hive::chain::feed_history_index)
HIVE_DEFINE_TYPE_REGISTRAR_REGISTER_TYPE(hive::chain::convert_request_index)
HIVE_DEFINE_TYPE_REGISTRAR_REGISTER_TYPE(hive::chain::collateralized_convert_request_index)