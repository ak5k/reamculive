add_library(
	reamculive OBJECT
	csurf_main.cpp
	csurf_mcu.cpp
	)

if(WIN32)
	target_sources(reamculive PRIVATE res.rc)
endif()
