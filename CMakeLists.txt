cmake_minimum_required(VERSION 3.16)

project(dprive_ml)
set(CMAKE_CXX_STANDARD 17)

find_package(Boost REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})
message(${Boost_INCLUDE_DIRS})

find_package(OpenFHE REQUIRED)

set( CMAKE_CXX_FLAGS ${OpenFHE_CXX_FLAGS} )

set(OpenFHE_INCLUDE /projectnb/he/seyda/openfhe-development/build/install/include)
set(OpenFHE_LIBDIR "/projectnb/he/seyda/openfhe-development/build/install/lib")

include_directories(${OPENMP_INCLUDES})
include_directories(${OpenFHE_INCLUDE})
link_directories(${OpenFHE_LIBDIR})
link_directories(${OPENMP_LIBRARIES})

include_directories( /projectnb/he/seyda/openfhe-development/build/install/include/openfhe/pke)
include_directories( /projectnb/he/seyda/openfhe-development/build/install/include/openfhe/core)
include_directories( /projectnb/he/seyda/openfhe-development/build/install/include/openfhe/binfhe)
include_directories( /projectnb/he/seyda/openfhe-development/build/install/include/openfhe)


if (BUILD_STATIC)
    set(CMAKE_EXE_LINKER_FLAGS "${OpenFHE_EXE_LINKER_FLAGS} -static")
    link_libraries(${OpenFHE_STATIC_LIBRARIES})
else ()
    set(CMAKE_EXE_LINKER_FLAGS ${OpenFHE_EXE_LINKER_FLAGS})
    link_libraries(${OpenFHE_SHARED_LIBRARIES})
endif ()

# add_executable(lr_nag lr_nag.cpp enc_matrix.cpp enc_matrix.h data_io.cpp data_io.h lr_types.h pt_matrix.cpp pt_matrix.h utils.cpp utils.h lr_train_funcs.cpp lr_train_funcs.h parameters.h)
# add_executable(cheb_analysis cheb_analysis.cpp enc_matrix.cpp enc_matrix.h data_io.cpp data_io.h lr_types.h pt_matrix.cpp pt_matrix.h utils.cpp utils.h lr_train_funcs.cpp lr_train_funcs.h)

# Define a helper function to add executables and link libraries
function(add_my_executable name)
    add_executable(${name} ${ARGN} ${SHARED_SOURCES})
    target_link_libraries(${name} PRIVATE ${OpenFHE_LIBDIR}/libOPENFHEpke.so.1.2.3 ${OpenFHE_LIBDIR}/libOPENFHEcore.so.1.2.3 ${OpenFHE_LIBDIR}/libOPENFHEbinfhe.so.1.2.3)
endfunction()

# Define shared source files
set(SHARED_SOURCES
        enc_matrix.cpp enc_matrix.h
        data_io.cpp data_io.h
        pt_matrix.cpp pt_matrix.h
        utils.cpp utils.h
        lr_train_funcs.cpp lr_train_funcs.h
        parameters.h
        PlateauLRScheduler.hpp
        #polynomial-approximation.hpp
        #minimax.hpp
)

# Add executables
add_my_executable(lr_nag lr_nag.cpp)

add_my_executable(lr_adam lr_adam.cpp)
add_my_executable(lr_adam_s lr_adam_s.cpp)

add_my_executable(lr_rms lr_rms.cpp)
add_my_executable(lr_rms_s lr_rms_s.cpp)

add_my_executable(lr_narms lr_narms.cpp)
add_my_executable(lr_nadam lr_nadam.cpp)


# ADD src
add_subdirectory(train_data)
