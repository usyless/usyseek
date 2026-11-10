#include "check.hpp"

#include <usyseek/torch_index.hpp>

using namespace usyseek;

int main() {
    CHECK(torch_index_url("cuda", "cu126") == "https://download.pytorch.org/whl/cu126");
    CHECK(torch_index_url("cuda", "cu130") == "https://download.pytorch.org/whl/cu130");
    CHECK(torch_index_url("cuda", "cu134") == "https://download.pytorch.org/whl/cu134");

    CHECK(torch_index_url("cuda", "") == "https://download.pytorch.org/whl/cu126");
    CHECK(torch_index_url("cuda", "cu") == "https://download.pytorch.org/whl/cu126");
    CHECK(torch_index_url("cuda", "12.6") == "https://download.pytorch.org/whl/cu126");
    CHECK(torch_index_url("cuda", "cu12x") == "https://download.pytorch.org/whl/cu126");
    CHECK(torch_index_url("cuda", "rocm6.4") == "https://download.pytorch.org/whl/cu126");
    CHECK(torch_index_url("cuda", "cu126/../cpu") == "https://download.pytorch.org/whl/cu126");
    CHECK(torch_index_url("cuda", " cu128") == "https://download.pytorch.org/whl/cu126");

    CHECK(torch_index_url("cuda", "CU128") == "https://download.pytorch.org/whl/cu128");

    CHECK(torch_index_url("cpu", "cu130") == "https://download.pytorch.org/whl/cpu");

    for (const std::string_view v : cuda_versions) {
        CHECK(normalize_cuda_version(v) == v);
    }
}
