# CPackProjectConfig.cmake —— cpack 打包时引入的配置脚本（W-08）
#
# 为什么需要这个文件：归档名要区分 Debug / Release（两者的内容不同：bin/Debug 与
# bin/Release），可是 CPACK_PACKAGE_FILE_NAME 在 configure 阶段拿不到多配置生成器的
# 配置，*也不*支持生成器表达式 —— 写成那种尖括号表达式时，CPack 会把字面量当目录名，
# 而 '<' '>' 在 Windows 上是非法字符，于是直接以「Problem creating temporary
# directory」失败（实测，不是推测）。
#
# CPack 在打包时（按 -C 给出配置之后）才 include 本文件，此刻 CPACK_BUILD_CONFIG 已就绪；
# 这是唯一能按配置改名而不用重复调用 cpack 的钩子。
#
# 注：前缀/版本/生成器等仍由根 CMakeLists.txt 的 CPACK_* 变量给出，本文件只负责文件名。
# 本文件也是 conan `exports_sources` 的 cmake/* 覆盖范围之内。

if(CPACK_BUILD_CONFIG)
    set(CPACK_PACKAGE_FILE_NAME
        "piplugin-${CPACK_PACKAGE_VERSION}-${CPACK_BUILD_CONFIG}-${CPACK_SYSTEM_NAME}")
else()
    set(CPACK_PACKAGE_FILE_NAME "piplugin-${CPACK_PACKAGE_VERSION}-${CPACK_SYSTEM_NAME}")
endif()
