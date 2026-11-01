#pragma once

// =============================================================================
// tests/config_test_util.h - CCU-FOTA 配置测试夹具 (CCU-FOTA-DSN-CR-004)
// =============================================================================
// RAII 临时目录，用于构造 ccu-framework-config 六阶段解析所需的 configRoots
// 与 conf.d/<svc>.yaml 场景，镜像框架 config_test_util.h 的模式（该头为框架私有，
// 未随安装包发布，故 FOTA 自带一份）。

#include <filesystem>
#include <fstream>
#include <string>
#include <cstdlib>

namespace ccu_fota_test {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;

    TempDir() {
        auto base = fs::temp_directory_path();
        std::string name = "ccu-fota-cfg-";
        for (int i = 0; i < 8; ++i) name.push_back('a' + (std::rand() % 26));
        path = base / name;
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    // 创建子目录（如 "conf.d"），返回完整路径。
    fs::path mkdir(const std::string& rel) {
        fs::path p = path / rel;
        fs::create_directories(p);
        return p;
    }

    // 将 content 写入相对路径。
    fs::path writeFile(const std::string& rel, const std::string& content) {
        fs::path p = path / rel;
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        if (!f) throw std::runtime_error("cannot write " + p.string());
        f << content;
        f.close();
        return p;
    }

    // 标准公共基线 common.yaml（common.log.*）。
    void writeCommonYaml() {
        writeFile("common.yaml",
            "common:\n"
            "  log:\n"
            "    schema_version: 1\n"
            "    level: INFO\n"
            "    strict: false\n"
            "    async:\n"
            "      enabled: true\n"
            "      queue_size: 4096\n"
            "      flush_interval_ms: 1000\n"
            "    console:\n"
            "      enabled: true\n"
            "    file:\n"
            "      enabled: false\n"
            "    redact:\n"
            "      identifiers: mask\n"
            "      payload_max_bytes: 256\n"
            "    format: standard\n");
    }
};

} // namespace ccu_fota_test
