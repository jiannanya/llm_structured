{
  "targets": [
    {
      "target_name": "addon",
      "sources": [
        "src/addon.cc",
        "../cpp/src/llm_structured.cpp"
      ],
      "include_dirs": [
        "../cpp/include"
      ],
      "defines": ["NAPI_VERSION=8", "NOMINMAX"],
      "cflags_cc": ["-std=c++17"],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "AdditionalOptions": ["/std:c++17", "/utf-8", "/EHsc"]
        }
      }
    }
  ]
}
