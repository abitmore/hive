if( NOT HIVE_CONVERTER_BUILD )

  message( AUTHOR_WARNING "Building without hive converter" )

else()

  if( BUILD_HIVE_TESTNET )
    message( WARNING "This tool is not intended to be used in testnet" )
  endif()

  ADD_HIVE_EXECUTABLE ( NAME blockchain_converter
                        SOURCES main.cpp converter.cpp
                                plugins/base/conversion_plugin.cpp
                                plugins/block_log_conversion/block_log_conversion_plugin.cpp
                                plugins/node_based_conversion/node_based_conversion_plugin.cpp
                                plugins/iceberg_generate/iceberg_generate_plugin.cpp
                        LIBRARIES hive_utilities hive_protocol hive_chain appbase fc )

endif()
