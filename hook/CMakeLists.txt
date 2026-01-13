add_library(hook
	SHARED
		src/main.cpp
		src/hook.cpp
		src/ConfigHandler.cpp
		src/MemoryUtils.cpp
		src/rpc-client/discord_manager.cpp
		src/rpc-client/levels.cpp
		src/rpc-client/rpc.cpp
		src/SigScanner.cpp
)

target_link_libraries(hook
	PRIVATE
		Winmm
		d3d9
		delayimp
		MinHook
		discord-rpc
)

target_include_directories(hook
	PRIVATE
		include
)

target_link_options(hook
	PRIVATE
		"/DELAYLOAD:discord-rpc.dll"
)

add_custom_command(
	TARGET hook
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
			${CMAKE_SOURCE_DIR}/external/discord-rpc/bin/discord-rpc.dll
			$<TARGET_FILE_DIR:hook>
)

set_target_properties(hook PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RATATAR_OUTPUT_DIR}"
)