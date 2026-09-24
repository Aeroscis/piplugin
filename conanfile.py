import os

from conan import ConanFile
from conan.errors import ConanException
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout

# Conan 选项与 CMake 缓存选项同名（PI_PLUGIN_BUILD_*），generate() 中按下述排除集整批转发给 CMake。
# shared/fPIC 是 Conan 打包语义选项：当前 CMake 硬编码核心为 SHARED、adapter kit 为 STATIC，
# 故不映射（转发了也不生效，徒增无谓缓存变量）；将来接入 BUILD_SHARED_LIBS 后移出排除集即可。
_CMAKE_EXCLUDED_OPTIONS = ("shared", "fPIC")

# 各测试件对 adapter kit 的需求（经 CMakeLists.txt 逐一核实；None = 仅依赖核心或直连 conan 依赖）
_TEST_ADAPTER_NEEDS = {
    "PI_PLUGIN_BUILD_TEST_HOST": None,            # imgui 测试宿主：直连 imgui(conan)，无需 adapter kit
    "PI_PLUGIN_BUILD_TEST_HOST_QT": "IMGUI",      # qt 测试宿主：渲染 imgui 插件 -> 需要 imgui adapter kit
    "PI_PLUGIN_BUILD_HEADLESS_HOST": None,        # headless 宿主：仅核心
    "PI_PLUGIN_BUILD_TEST_HOST_MULTI": None,      # 多插件同进程宿主（APP-08）：仅核心 + L0 kit
    "PI_PLUGIN_BUILD_TEST_HOST_EVENTS": None,     # 事件宿主（APP-06）：仅核心 + L0/events kit
    "PI_PLUGIN_BUILD_TEST_PLUGIN": "QT",          # qt 测试插件：需要 qt adapter kit
    "PI_PLUGIN_BUILD_TEST_PLUGIN_IMGUI": "IMGUI",  # imgui 测试插件：需要 imgui adapter kit
    "PI_PLUGIN_BUILD_TEST_PLUGIN_BADVERSION": None,   # 坏版本测试插件（BLK-03）：仅核心
    "PI_PLUGIN_BUILD_TEST_PLUGIN_GUIREQUIRED": None,  # GUI-required 测试插件（ECO-08）：仅核心
    "PI_PLUGIN_BUILD_TEST_PLUGIN_SERVICE": None,      # 服务测试插件（APP-07）：仅核心
    "PI_PLUGIN_BUILD_TEST_PLUGIN_EVENTS": None,       # 事件测试插件（APP-06）：仅核心
    "PI_PLUGIN_BUILD_UNIT_TESTS": None,               # 核心单测（BLK-06）：仅核心
    "PI_PLUGIN_BUILD_UNIT_CPP_TESTS": None,           # C++ RAII 层测试（APP-05）：仅核心
}

# 各测试宿主对宿主 kit 的需求（三个测试宿主都已改用宿主 kit；
# 关闭对应 kit 时 CMake 侧会禁用该宿主，这里显式报错而不是静默降级）
#   值 = 需要的宿主 kit 分开关名，对应 PI_PLUGIN_BUILD_HOST_KIT_<名>
_TEST_HOST_KIT_NEEDS = {
    "PI_PLUGIN_BUILD_TEST_HOST": ("CORE", "DX11"),     # imgui 宿主：L0 会话 + L1 dx11 交换链
    "PI_PLUGIN_BUILD_TEST_HOST_QT": ("CORE", "QT"),    # qt 宿主：L0 会话 + L1 qt 嵌入区域
    "PI_PLUGIN_BUILD_HEADLESS_HOST": ("CORE",),        # headless 宿主：仅 L0 会话
    "PI_PLUGIN_BUILD_TEST_HOST_MULTI": ("CORE",),      # 多插件宿主（APP-08）：仅 L0 会话
    "PI_PLUGIN_BUILD_TEST_HOST_EVENTS": ("CORE", "EVENTS"),  # 事件宿主（APP-06）：L0 会话 + 事件路由
}


class PiPluginConan(ConanFile):
    name = "piplugin"
    version = "0.5.0"
    license = "MIT"
    author = "Aeroscis"
    url = "https://gitee.com/Aeroscis/piplugin"
    description = "Cross-platform plugin framework with COM-style C ABI"
    topics = ("plugin", "framework", "c", "ffi")
    settings = "os", "compiler", "build_type", "arch"

    # ------------------------- 开关树（与 CMake 选项同名，一一对应）-------------------------
    # 结构：核心（必编，无开关）
    #      + adapter kits   ：总开关 PI_PLUGIN_BUILD_ADAPTERS          + 每框架分开关 PI_PLUGIN_BUILD_ADAPTER_*
    #      + host kits      ：总开关 PI_PLUGIN_BUILD_HOST_KITS         + 每层分开关 PI_PLUGIN_BUILD_HOST_KIT_*
    #        （宿主侧机制库；L0 core 已抽出，三个测试宿主都已改用它）
    #      + tests          ：总开关 PI_PLUGIN_BUILD_TESTS             + 每测试件分开关 PI_PLUGIN_BUILD_TEST_*
    # 依赖：开任一需要 imgui 的开关 -> requirements() 自动拉取；总开关关死 -> 下层分开关有效关闭
    #      （有效状态计算见 _adapter_enabled/_host_kit_enabled/_test_enabled，与 CMake 侧守卫语义一致）。
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        # adapter kits（产品部件，默认全开）
        "PI_PLUGIN_BUILD_ADAPTERS": [True, False],       # 总开关
        "PI_PLUGIN_BUILD_ADAPTER_QT": [True, False],     # 分开关：Qt5 为本地安装，非 conan 依赖
        "PI_PLUGIN_BUILD_ADAPTER_IMGUI": [True, False],  # 分开关：依赖 conan imgui
        # host kits（宿主侧 kit，产品部件，默认全开；无 conan 依赖）
        "PI_PLUGIN_BUILD_HOST_KITS": [True, False],      # 总开关
        "PI_PLUGIN_BUILD_HOST_KIT_CORE": [True, False],  # 分开关：L0 会话库（仅依赖核心）
        "PI_PLUGIN_BUILD_HOST_KIT_EVENTS": [True, False],  # 分开关：宿主侧事件路由 piplugin_events（APP-06）
        "PI_PLUGIN_BUILD_HOST_KIT_QT": [True, False],    # 分开关：L1 Qt 嵌入区域（Qt5 本地安装 + L0）
        "PI_PLUGIN_BUILD_HOST_KIT_DX11": [True, False],  # 分开关：L1 DX11 嵌入胶水（Windows）
        # tests（测试件，默认全开；conan create 打包时建议 -o PI_PLUGIN_BUILD_TESTS=False）
        # examples（ECO-03：可构建的最小示范；只依赖公开 API，不进包）
        "PI_PLUGIN_BUILD_EXAMPLES": [True, False],       # 总开关
        "PI_PLUGIN_BUILD_TESTS": [True, False],          # 总开关
        "PI_PLUGIN_BUILD_UNIT_TESTS": [True, False],     # 核心回归单测（ctest 的 unit 用例）
        "PI_PLUGIN_BUILD_UNIT_CPP_TESTS": [True, False],  # C++ RAII 层测试（ctest 的 unit_cpp 用例）
        "PI_PLUGIN_BUILD_TEST_HOST": [True, False],      # imgui 测试宿主（依赖 imgui）
        "PI_PLUGIN_BUILD_TEST_HOST_QT": [True, False],   # qt 测试宿主（依赖 Qt5 + imgui adapter kit）
        "PI_PLUGIN_BUILD_HEADLESS_HOST": [True, False],  # headless 测试宿主（仅依赖核心）
        "PI_PLUGIN_BUILD_TEST_HOST_MULTI": [True, False],  # 多插件同进程验收宿主（仅依赖核心 + L0 kit）
        "PI_PLUGIN_BUILD_TEST_HOST_EVENTS": [True, False],  # 事件机制验收宿主（仅依赖核心 + L0/events kit）
        "PI_PLUGIN_BUILD_TEST_PLUGIN": [True, False],     # qt 测试插件（依赖 Qt5 + qt adapter kit）
        "PI_PLUGIN_BUILD_TEST_PLUGIN_IMGUI": [True, False],  # imgui 测试插件（依赖 imgui + imgui adapter kit）
        "PI_PLUGIN_BUILD_TEST_PLUGIN_BADVERSION": [True, False],  # 声明不兼容 api_version 的测试插件（BLK-03 负向用例，仅依赖核心）
        "PI_PLUGIN_BUILD_TEST_PLUGIN_GUIREQUIRED": [True, False],  # 声明 HOST_UI REQUIRED 的测试插件（ECO-08 负向用例，仅依赖核心）
        "PI_PLUGIN_BUILD_TEST_PLUGIN_SERVICE": [True, False],  # 服务测试插件（APP-07，仅依赖核心）
        "PI_PLUGIN_BUILD_TEST_PLUGIN_EVENTS": [True, False],   # 事件测试插件（APP-06，仅依赖核心）
    }
    default_options = {
        "shared": True,
        "fPIC": True,
        "PI_PLUGIN_BUILD_ADAPTERS": True,
        "PI_PLUGIN_BUILD_ADAPTER_QT": True,
        "PI_PLUGIN_BUILD_ADAPTER_IMGUI": True,
        "PI_PLUGIN_BUILD_HOST_KITS": True,
        "PI_PLUGIN_BUILD_HOST_KIT_CORE": True,
        "PI_PLUGIN_BUILD_HOST_KIT_EVENTS": True,
        "PI_PLUGIN_BUILD_HOST_KIT_QT": True,
        "PI_PLUGIN_BUILD_HOST_KIT_DX11": True,
        "PI_PLUGIN_BUILD_EXAMPLES": True,
        "PI_PLUGIN_BUILD_TESTS": True,
        "PI_PLUGIN_BUILD_UNIT_TESTS": True,
        "PI_PLUGIN_BUILD_UNIT_CPP_TESTS": True,
        "PI_PLUGIN_BUILD_TEST_HOST": True,
        "PI_PLUGIN_BUILD_TEST_HOST_QT": True,
        "PI_PLUGIN_BUILD_HEADLESS_HOST": True,
        "PI_PLUGIN_BUILD_TEST_HOST_MULTI": True,
        "PI_PLUGIN_BUILD_TEST_HOST_EVENTS": True,
        "PI_PLUGIN_BUILD_TEST_PLUGIN": True,
        "PI_PLUGIN_BUILD_TEST_PLUGIN_IMGUI": True,
        "PI_PLUGIN_BUILD_TEST_PLUGIN_BADVERSION": True,
        "PI_PLUGIN_BUILD_TEST_PLUGIN_GUIREQUIRED": True,
        "PI_PLUGIN_BUILD_TEST_PLUGIN_SERVICE": True,
        "PI_PLUGIN_BUILD_TEST_PLUGIN_EVENTS": True,
    }

    # 根文档（LICENSE / README.md / CHANGELOG.md）一并导出：CMake install 规则会把它们
    # 装进分发产物（W-08），而 package() 走的就是 cmake.install()，所以这三份在 conan 的
    # 构建目录里也必须存在 —— 漏掉时的症状是 package() 阶段直接失败：
    #   CMake Error: file INSTALL cannot find ".../b/LICENSE": File exists.
    # 顺带把 build.md #7 的遗留项（conan 包不随包带 LICENSE）一起解决。
    exports_sources = ("CMakeLists.txt", "cmake/*", "include/*", "src/*", "adapters/*",
                       "host_kits/*", "examples/*", "tests/*",
                       "LICENSE", "README.md", "CHANGELOG.md")
    # CMakeToolchain 不在 generators 声明：需要在 generate() 手动实例化以注入自定义 cache 变量。
    # （Conan 禁止同一生成器既声明又手动实例化；CMakeDeps 的依赖查找路径经
    #   conan_cmakedeps_paths.cmake 由工具链在 configure 时包含，与生成顺序无关）
    generators = "CMakeDeps"

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def layout(self):
        cmake_layout(self)

    # ------------------------- 有效状态计算（单一事实来源）-------------------------
    # Conan 2 在 configure() 阶段禁止改写选项值（应用 -o 后即 freeze），
    # 因此不做选项归一化/联动，而是在使用处计算"有效状态"：总开关 AND 分开关。
    # generate() 仍把原始选项原样透传给 CMake，门控由 CMake 侧同构的守卫完成
    #（adapters/CMakeLists.txt 的 if(NOT PI_PLUGIN_BUILD_ADAPTERS) return() 等）。

    def _adapter_enabled(self, kit):
        """adapter kit 有效状态：总开关 PI_PLUGIN_BUILD_ADAPTERS AND 分开关 PI_PLUGIN_BUILD_ADAPTER_<kit>"""
        return bool(self.options.PI_PLUGIN_BUILD_ADAPTERS) and bool(getattr(self.options,
                                                                    f"PI_PLUGIN_BUILD_ADAPTER_{kit}"))

    def _host_kit_enabled(self, kit):
        """宿主 kit 有效状态：总开关 PI_PLUGIN_BUILD_HOST_KITS AND 分开关 PI_PLUGIN_BUILD_HOST_KIT_<kit>"""
        return bool(self.options.PI_PLUGIN_BUILD_HOST_KITS) and bool(getattr(self.options,
                                                                     f"PI_PLUGIN_BUILD_HOST_KIT_{kit}"))

    def _test_enabled(self, test_switch):
        """测试件有效状态：总开关 PI_PLUGIN_BUILD_TESTS AND 分开关；无需 adapter 的测试件不在表内"""
        return bool(self.options.PI_PLUGIN_BUILD_TESTS) and bool(getattr(self.options, test_switch))

    def validate(self):
        # 真正无法自洽的矛盾：有效开启的测试件需要某 adapter kit，而该 kit 有效关闭
        #（总开关或分开关任一关闭）。不静默跳过（那正是要消灭的"静默降级"），显式报错
        problems = []
        for test_switch, adapter in _TEST_ADAPTER_NEEDS.items():
            if adapter and self._test_enabled(test_switch) and not self._adapter_enabled(adapter):
                problems.append(f"{test_switch} requires PI_PLUGIN_BUILD_ADAPTERS=True "
                                f"and PI_PLUGIN_BUILD_ADAPTER_{adapter}=True")
        # 同理：测试宿主需要宿主 kit（各自需要哪几个见 _TEST_HOST_KIT_NEEDS）
        for test_switch, kits in _TEST_HOST_KIT_NEEDS.items():
            if not self._test_enabled(test_switch):
                continue
            if not all(self._host_kit_enabled(kit) for kit in kits):
                need = ", ".join(f"PI_PLUGIN_BUILD_HOST_KIT_{kit}=True" for kit in kits)
                problems.append(f"{test_switch} requires PI_PLUGIN_BUILD_HOST_KITS=True and {need}")
        if problems:
            raise ConanException(
                "; ".join(problems)
                + ". Enable the required kit switches or disable those test switches.")

    def requirements(self):
        # 家族根层：无条件依赖（结果码 / GUID / PiNativeWindow / IPiUnknown / ABI 管线宏）。
        # header-only 包，不参与构建类型或架构的 package_id。
        #
        # transitive_headers=True 不是可选的美化：框架的**公开头文件**
        # （include/piplugin/pi_plugin_types.h 等）里写着 #include <pibase/pi_base.h>，
        # 消费方编译期必须能找到它；而 Conan 默认的传播规则（internal/model/requires.py
        # 的 Requirement.transform_downstream：src -> shared/unknown -> header 这条路径）
        # 会给下游生成 headers=False 的 require —— 于是消费方的依赖图里虽然还有
        # pibase 这个节点，CMakeDeps 却不为它生成 pibase-config.cmake，
        # 也没有任何 target 携带它的 include 目录，消费方在
        # #include <pibase/pi_base.h> 处直接 C1083。声明这个 trait 之后，
        # 头文件需求随 <piplugin> 一起传到消费方，消费方不必自己去 require pibase。
        self.requires("pibase/0.1.0", transitive_headers=True)

        # 依赖自动管理：任一需要 imgui 的部件有效开启即自动拉取（Qt5 为本地安装，非 conan 依赖）
        # - imgui adapter kit 链接 imgui::imgui
        # - imgui 测试宿主（PI_PLUGIN_BUILD_TEST_HOST）直连 imgui，但不依赖 adapter kit
        need_imgui = self._adapter_enabled("IMGUI") or self._test_enabled("PI_PLUGIN_BUILD_TEST_HOST")
        if need_imgui:
            self.requires("imgui/1.92.8", transitive_libs=True)

    def generate(self):
        toolchain = CMakeToolchain(self)

        # 1) 优先读取自定义 conf（建议写在标准 profile 中，如 ~/.conan2/profiles/
        #    MSVC2022-amd64-Cpp17-Debug 的 [conf] 段），也可用 -c 在命令行临时覆盖：
        #      [conf]
        #      user.cmake_ext:cmake_prefix_path=C:/Qt/6.10.0/msvc2022_64
        #      user.cmake_ext:cmake_module_path=C:/dev/install/msvc2022_64
        #      user.cmake_ext:cmake_install_prefix=C:/dev/install/msvc2022_64
        cmake_prefix_path = self.conf.get("user.cmake_ext:cmake_prefix_path", check_type=str)
        cmake_module_path = self.conf.get("user.cmake_ext:cmake_module_path", check_type=str)
        cmake_install_prefix = self.conf.get("user.cmake_ext:cmake_install_prefix", check_type=str)

        # 2) conf 未配置时，降级读取 CMake 原生支持的同名环境变量
        if not cmake_prefix_path:
            cmake_prefix_path = os.getenv("CMAKE_PREFIX_PATH", "")
        if not cmake_module_path:
            cmake_module_path = os.getenv("CMAKE_MODULE_PATH", "")
        if not cmake_install_prefix:
            cmake_install_prefix = os.getenv("CMAKE_INSTALL_PREFIX", "")

        # 3) 有值才注入，避免无谓覆盖；注入写入 CMakePresets.json 的 cacheVariables，
        #    configure 时以 -D 传入（CMakeDeps 注入的依赖查找路径在前，互不冲突）
        if cmake_prefix_path:
            toolchain.cache_variables["CMAKE_PREFIX_PATH"] = cmake_prefix_path
        if cmake_module_path:
            toolchain.cache_variables["CMAKE_MODULE_PATH"] = cmake_module_path
        if cmake_install_prefix:
            toolchain.cache_variables["CMAKE_INSTALL_PREFIX"] = cmake_install_prefix

        # 4) 开关整批转发：选项名与 CMake 缓存选项同名，Conan 会把 bool 转成 ON/OFF
        for option_name, option_value in self.options.items():
            if option_name in _CMAKE_EXCLUDED_OPTIONS:
                continue
            toolchain.cache_variables[option_name] = option_value

        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()  # 开关已全部经 generate() 写入 presets cacheVariables
        cmake.build()

    def package(self):
        # 走 CMake install 规则（Modern CMake：install(TARGETS/FILES/EXPORT)），
        # 打包内容 = 核心库 + adapter kits（若开启）+ 各自头文件 + cmake config；
        # 测试件没有任何 install 规则，天然不进包。
        cmake = CMake(self)
        cmake.install()

    def _packaged(self, stem):
        """该库是否真的进了包？

        package_info() 必须描述**包里有什么**，而不是选项说了什么：CMake 侧可以
        静默禁用某个 target（例如找不到 Qt5 时 Qt 系列目标整批禁用，见 ECO-05），
        此时若仍然声明该组件，CMakeDeps 会给每个消费方报
        "Library 'piplugin_host_qtd' not found in package" 而配置失败
        —— 由 scripts/verify_package.ps1 的消费方用例抓出来。
        """
        folder = self.package_folder
        if not folder:
            return False
        suffix = "d" if self.settings.build_type == "Debug" else ""
        names = [f"{stem}{suffix}.lib", f"{stem}{suffix}.a",
                 f"{stem}{suffix}.so", f"{stem}{suffix}.dylib",
                 f"{stem}{suffix}.dll"]
        for sub in (f"lib/{self.settings.build_type}", f"bin/{self.settings.build_type}"):
            for name in names:
                if os.path.exists(os.path.join(folder, sub, name)):
                    return True
        return False

    def package_info(self):
        # 产物布局与 install 规则一致：lib/<build_type>、bin/<build_type>；
        # Debug 构建的库名带全局 "d" 后缀（GLOBAL_PROJECT_BUILD_TYPE_SUFFIX）
        suffix = "d" if self.settings.build_type == "Debug" else ""
        self.cpp_info.libdirs = [f"lib/{self.settings.build_type}"]
        self.cpp_info.bindirs = [f"bin/{self.settings.build_type}"]
        # 与安装树导出的目标名对齐（NAMESPACE pi::），Conan 消费方与裸 CMake 消费方目标名一致
        core = self.cpp_info.components["piplugin"]
        core.libs = [f"piplugin{suffix}"]
        # 家族根层（pibase）：头文件来自它，核心的链接接口里也有 pi::base。
        # 不声明的话，CMakeDeps 生成的 pi::plugin 既不带 pi::base，也不带它的
        # include 目录，消费者会在 #include <pibase/pi_base.h> 处失败。
        #
        # 写法必须是 `pibase::pibase`：pibase 包**没有组件**（它只有一个 root
        # cpp_info），而 pkg::pkg 形式正好指向那个 root cpp_info，其 CMake target 名
        # 由它自己的 cmake_target_name 属性给出，即 pi::base。另两种写法都不行：
        #   - 裸名 "pibase" 会被当成**本包的内部组件**，conan create 直接报
        #     "Internal components not found"；
        #   - "pibase::base" 指向一个不存在的组件，CMakeDeps 生成 piplugin 数据时抛
        #     "Component 'pibase::base' not found in 'pibase' package requirement"。
        core.requires = ["pibase::pibase"]
        core.set_property("cmake_target_name", "pi::plugin")

        # 宿主 kit L0（宿主侧机制库；仅依赖核心，无第三方依赖）
        if self._host_kit_enabled("CORE") and self._packaged("piplugin_host"):
            comp = self.cpp_info.components["piplugin_host"]
            comp.libs = [f"piplugin_host{suffix}"]
            comp.requires = ["piplugin"]
            comp.set_property("cmake_target_name", "pi::plugin_host")

        # 宿主 kit L1 Qt 嵌入区域（依赖 L0 + 本地安装的 Qt5，非 conan 依赖）
        if self._host_kit_enabled("QT") and self._packaged("piplugin_host_qt"):
            comp = self.cpp_info.components["piplugin_host_qt"]
            comp.libs = [f"piplugin_host_qt{suffix}"]
            comp.requires = ["piplugin", "piplugin_host"]
            comp.set_property("cmake_target_name", "pi::plugin_host_qt")

        # 宿主侧事件路由（APP-06；可选糖，仅依赖核心）
        if self._host_kit_enabled("EVENTS") and self._packaged("piplugin_events"):
            comp = self.cpp_info.components["piplugin_events"]
            comp.libs = [f"piplugin_events{suffix}"]
            comp.requires = ["piplugin"]
            comp.set_property("cmake_target_name", "pi::plugin_events")

        # 宿主 kit L1 DX11 嵌入胶水（仅 Windows；只依赖核心）
        if self._host_kit_enabled("DX11") and self._packaged("piplugin_host_dx11"):
            comp = self.cpp_info.components["piplugin_host_dx11"]
            comp.libs = [f"piplugin_host_dx11{suffix}"]
            comp.requires = ["piplugin"]
            comp.set_property("cmake_target_name", "pi::plugin_host_dx11")
            # 同 imgui 套件：静态库 PUBLIC 链接的平台库，CMakeDeps 需要显式 system_libs
            if self.settings.get_safe("os") == "Windows":
                comp.system_libs = ["d3d11", "dxgi"]

        if self._adapter_enabled("IMGUI") and self._packaged("piplugin_imgui"):
            comp = self.cpp_info.components["piplugin_imgui"]
            comp.libs = [f"piplugin_imgui{suffix}"]
            # 外部包引用必须写 包名::组件名；无组件的包用 包名::包名 兜底到根 cpp_info
            comp.requires = ["piplugin", "imgui::imgui"]
            comp.set_property("cmake_target_name", "pi::plugin_imgui")
            # 静态套件把平台库以 PRIVATE 链接（user32/d3d11/dxgi/d3dcompiler），但**静态库的
            # 消费方在链接期仍然需要它们**：CMake 的导出 target 会自动带上，CMakeDeps 只能靠
            # cpp_info.system_libs —— 漏了就在消费方报
            # "unresolved external symbol D3D11CreateDeviceAndSwapChain"（实测）。
            if self.settings.get_safe("os") == "Windows":
                comp.system_libs = ["user32", "d3d11", "dxgi", "d3dcompiler"]
        elif self._test_enabled("PI_PLUGIN_BUILD_TEST_HOST"):
            # imgui 仅为测试宿主拉取（adapter kit 未开启）：测试件不进包，但其依赖须在
            # 包信息中可见，否则 Conan 组件一致性检查会拒绝该变体
            self.cpp_info.requires = ["imgui::imgui"]
        if self._adapter_enabled("QT") and self._packaged("piplugin_qt"):
            # 注意：Qt5 是本地安装（非 conan 依赖），消费方需自行保证 find_package(Qt5) 可达
            comp = self.cpp_info.components["piplugin_qt"]
            comp.libs = [f"piplugin_qt{suffix}"]
            comp.requires = ["piplugin"]
            comp.set_property("cmake_target_name", "pi::plugin_qt")

        # 组件**不继承**包级 libdirs/bindirs：必须逐个设置。否则 CMakeDeps 生成的是
        # <pkg>/lib（默认值），而库里实际在 <pkg>/lib/Debug，消费方 find_package 时
        # 直接报 "Library 'piplugin_host_qtd' not found in package" —— 由
        # scripts/verify_package.ps1 的消费方用例抓出来（ECO-04）。
        #
        # includedirs 同理：Conan 消费方拿到的是 CMakeDeps 生成的 target（不是我们导出的
        # target 文件），它的搜索路径只来自 cpp_info。各 kit 的头文件按 CMake 的
        # INSTALL_INTERFACE 平铺在 include/piplugin/<host_kits|adapters>/<kit>/ 下，
        # 所以要把这些目录逐个加到对应组件里，才能让消费方继续写
        # #include "pi_host_session.h" / "pi_imgui_view.h"（与构建树写法一致）。
        _kit_include_dirs = {
            "piplugin_host": "include/piplugin/host_kits/core",
            "piplugin_events": "include/piplugin/host_kits/events",
            "piplugin_host_qt": "include/piplugin/host_kits/qt",
            "piplugin_host_dx11": "include/piplugin/host_kits/dx11",
            "piplugin_imgui": "include/piplugin/adapters/imgui",
            "piplugin_qt": "include/piplugin/adapters/qt",
        }
        for _name, _comp in self.cpp_info.components.items():
            _comp.libdirs = self.cpp_info.libdirs
            _comp.bindirs = self.cpp_info.bindirs
            _own = _kit_include_dirs.get(_name)
            _comp.includedirs = ["include", _own] if _own else ["include"]
