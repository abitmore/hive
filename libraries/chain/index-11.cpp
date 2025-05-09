#include <hive/chain/comment_object.hpp>
#include <hive/chain/dhf_objects.hpp>
#include <hive/chain/hive_objects.hpp>

#include <hive/chain/index.hpp>

#include <hive/chain/util/type_registrar_definition.hpp>

namespace hive { namespace chain {

void initialize_core_indexes_11( database& db )
{
  HIVE_ADD_CORE_INDEX(db, proposal_vote_index);
  HIVE_ADD_CORE_INDEX(db, comment_cashout_index);
  HIVE_ADD_CORE_INDEX(db, comment_cashout_ex_index);
  HIVE_ADD_CORE_INDEX(db, recurrent_transfer_index);
}

} }

HIVE_DEFINE_TYPE_REGISTRAR_REGISTER_TYPE(hive::chain::proposal_vote_index)
HIVE_DEFINE_TYPE_REGISTRAR_REGISTER_TYPE(hive::chain::comment_cashout_index)
HIVE_DEFINE_TYPE_REGISTRAR_REGISTER_TYPE(hive::chain::comment_cashout_ex_index)
HIVE_DEFINE_TYPE_REGISTRAR_REGISTER_TYPE(hive::chain::recurrent_transfer_index)