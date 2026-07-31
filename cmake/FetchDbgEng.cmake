# FetchDbgEng.cmake - puts the Debugging Tools for Windows runtime DLLs into
# third_party/bin/<arch> at configure time.
#
# Those DLLs (dbgeng, dbgcore, dbghelp, dbgmodel, msdia140, symsrv, srcsrv) are
# Microsoft's, so they are not committed to this repo. They are published by
# Microsoft on nuget.org as three packages that are just zips of the DLLs, so a
# fresh clone can pull exactly the pinned build with no Windows SDK install and
# no manual download step:
#
#   Microsoft.Debugging.Platform.DbgEng   dbgeng/dbgcore/dbghelp/dbgmodel/msdia140
#   Microsoft.Debugging.Platform.SymSrv   symsrv   (the srv*...*msdl symbol path)
#   Microsoft.Debugging.Platform.SrcSrv   srcsrv   (source-indexed stepping)
#
# Everything is version-pinned and SHA256-checked. Set -DODBG_FETCH_DBGENG=OFF
# to opt out and drop the DLLs into third_party/bin/<arch> yourself.

option(ODBG_FETCH_DBGENG "Download the Debugging Tools DLLs from nuget.org if missing" ON)

set(ODBG_DBGENG_VERSION "20260319.1511.0" CACHE STRING "Pinned Debugging Tools package version")

# Hashes belong to ODBG_DBGENG_VERSION - bump both together, or the download
# fails loudly rather than silently pulling something else.
set(_odbg_pkg_dbgeng_sha 875678516f9ceed4a1c8b9b106d165106fcaa38723ad7c584c47b95b231600de)
set(_odbg_pkg_symsrv_sha 8d24440267581038de0101b36c0d75daf7f063d86f3d6adca61038c236df7ce7)
set(_odbg_pkg_srcsrv_sha 9437e6bb9c7298bc8c15fd9125373816fcd6afb6061221c4ead85c976e1768c7)

# Downloads one nuget package and copies content/<nuget_arch>/*.dll into dest.
function(_odbg_fetch_pkg pkg_id sha nuget_arch dest)
    string(TOLOWER "${pkg_id}" pkg_lower)
    set(stage "${CMAKE_CURRENT_BINARY_DIR}/_dbgeng/${pkg_lower}-${ODBG_DBGENG_VERSION}")
    set(nupkg "${stage}.nupkg")

    if(NOT EXISTS "${stage}/content/${nuget_arch}")
        message(STATUS "Fetching ${pkg_id} ${ODBG_DBGENG_VERSION} from nuget.org")
        file(DOWNLOAD
            "https://api.nuget.org/v3-flatcontainer/${pkg_lower}/${ODBG_DBGENG_VERSION}/${pkg_lower}.${ODBG_DBGENG_VERSION}.nupkg"
            "${nupkg}"
            EXPECTED_HASH SHA256=${sha}
            STATUS dl_status
            TLS_VERIFY ON)
        list(GET dl_status 0 dl_code)
        if(NOT dl_code EQUAL 0)
            list(GET dl_status 1 dl_msg)
            message(FATAL_ERROR "Downloading ${pkg_id} failed: ${dl_msg}")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${nupkg}" DESTINATION "${stage}")
    endif()

    file(GLOB dlls "${stage}/content/${nuget_arch}/*.dll")
    if(NOT dlls)
        message(FATAL_ERROR "${pkg_id} has no DLLs under content/${nuget_arch}")
    endif()
    file(COPY ${dlls} DESTINATION "${dest}")
endfunction()

# arch_dir is our own naming (x64/x86, matching third_party/bin/<arch_dir>);
# nuget spells the 64-bit one "amd64".
function(odbg_provide_dbgeng arch_dir dest)
    if(arch_dir STREQUAL "x64")
        set(nuget_arch "amd64")
    else()
        set(nuget_arch "${arch_dir}")
    endif()

    set(required dbgeng.dll dbgcore.dll dbghelp.dll dbgmodel.dll msdia140.dll symsrv.dll srcsrv.dll)
    set(missing "")
    foreach(dll ${required})
        if(NOT EXISTS "${dest}/${dll}")
            list(APPEND missing "${dll}")
        endif()
    endforeach()

    if(NOT missing)
        return()
    endif()

    if(NOT ODBG_FETCH_DBGENG)
        message(FATAL_ERROR
            "Missing from ${dest}: ${missing}\n"
            "Either re-enable -DODBG_FETCH_DBGENG=ON, or copy them there from a "
            "Windows SDK 'Debugging Tools for Windows' install.")
    endif()

    file(MAKE_DIRECTORY "${dest}")
    _odbg_fetch_pkg(Microsoft.Debugging.Platform.DbgEng "${_odbg_pkg_dbgeng_sha}" "${nuget_arch}" "${dest}")
    _odbg_fetch_pkg(Microsoft.Debugging.Platform.SymSrv "${_odbg_pkg_symsrv_sha}" "${nuget_arch}" "${dest}")
    _odbg_fetch_pkg(Microsoft.Debugging.Platform.SrcSrv "${_odbg_pkg_srcsrv_sha}" "${nuget_arch}" "${dest}")
endfunction()
