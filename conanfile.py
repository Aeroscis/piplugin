from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy


class PiPluginFrameworkConan(ConanFile):
    name = "pipluginframework"
    version = "1.0.0"
    license = "MIT"
    author = "pipluginframework"
    url = "https://github.com/example/pipluginframework"
    description = "Cross-platform plugin framework with COM-style C ABI"
    topics = ("plugin", "framework", "c", "ffi")
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_tests": [True, False],
    }
    default_options = {
        "shared": True,
        "fPIC": True,
        "with_tests": True,
    }
    exports_sources = "CMakeLists.txt", "cmake/*", "include/*", "src/*", "adapters/*", "tests/*"
    generators = "CMakeToolchain", "CMakeDeps"

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        if self.options.with_tests:
            self.requires("imgui/1.92.8")

    def build(self):
        cmake = CMake(self)
        cmake.configure(variables={"PI_BUILD_TESTS": self.options.with_tests})
        cmake.build()

    def package(self):
        copy(self, "*.h", src=self.source_folder + "/include",
             dst=self.package_folder + "/include")
        copy(self, "*.lib", src=self.build_folder,
             dst=self.package_folder + "/lib", keep_path=False)
        copy(self, "*.dll", src=self.build_folder,
             dst=self.package_folder + "/bin", keep_path=False)
        copy(self, "*.so", src=self.build_folder,
             dst=self.package_folder + "/lib", keep_path=False)
        copy(self, "*.dylib", src=self.build_folder,
             dst=self.package_folder + "/lib", keep_path=False)
        copy(self, "*.a", src=self.build_folder,
             dst=self.package_folder + "/lib", keep_path=False)

    def package_info(self):
        self.cpp_info.libs = ["pipluginframework"]
