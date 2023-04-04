add_library(
	reamcusof OBJECT
	csurf_main.cpp
	csurf_mcu.cpp
	)

if(WIN32)
	target_sources(reamcusof PRIVATE res.rc)
endif()
