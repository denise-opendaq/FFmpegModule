include_guard(GLOBAL)

if (DAQMODULES_OPCUA_ENABLE_CLIENT)
    opendaq_append_required_boost_components(
        uuid
    )
endif()