add_executable(RatataR WIN32
	src/main.cpp
	src/RatataR.cpp
	resources/RatataR.rc
)

target_include_directories(RatataR
	PRIVATE
		include
)

target_link_libraries(RatataR
	PRIVATE
		Winmm
		kernel32
		user32
)

set_target_properties(RatataR PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RATATAR_OUTPUT_DIR}"
)