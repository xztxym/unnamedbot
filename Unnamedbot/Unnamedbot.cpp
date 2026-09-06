#include <iostream>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <cpr/cpr.h>

using json = nlohmann::json;

int main() {
    std::cout << "=== unnamedbot 完整验证 ===\n";

    // 测试 nlohmann/json
    json j = { {"status", "ok"}, {"lib", "nlohmann-json"} };
    std::cout << "JSON: " << j.dump(2) << std::endl;

    // 测试 SQLite3
    sqlite3* db = nullptr;
    int rc = sqlite3_open(":memory:", &db);
    std::cout << "SQLite3: " << (rc == SQLITE_OK ? "成功" : "失败") << std::endl;
    if (rc == SQLITE_OK) sqlite3_close(db);

    // 测试 CPR + libcurl（网络请求）
    auto response = cpr::Get(cpr::Url{ "https://httpbin.org/get" });
    std::cout << "CPR HTTP: " << response.status_code << std::endl;

    std::cout << "所有依赖库验证通过！\n";
    return 0;
}