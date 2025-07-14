// swift-tools-version:5.9
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

import PackageDescription

let version = "0.8.0.20250714"
let url = "https://ossci-ios.s3.amazonaws.com/executorch/"
let debug_suffix = "_debug"
let dependencies_suffix = "_with_dependencies"

func deliverables(_ dict: [String: [String: Any]]) -> [String: [String: Any]] {
  dict
    .reduce(into: [String: [String: Any]]()) { result, pair in
      let (key, value) = pair
      result[key] = value
      result[key + debug_suffix] = value
    }
    .reduce(into: [String: [String: Any]]()) { result, pair in
      let (key, value) = pair
      var newValue = value
      if key.hasSuffix(debug_suffix) {
        for (k, v) in value where k.hasSuffix(debug_suffix) {
          let trimmed = String(k.dropLast(debug_suffix.count))
          newValue[trimmed] = v
        }
      }
      result[key] = newValue.filter { !$0.key.hasSuffix(debug_suffix) }
    }
}

let products = deliverables([
  "backend_coreml": [
    "sha256": "a7643174c1f45276c0e9d6957af2b7980f13d6ec058812c242c46694912fc32f",
    "sha256" + debug_suffix: "48723434c6f6f409a609e11e076ac0c917829c9379ef4299c5743c1dd67e912c",
    "frameworks": [
      "Accelerate",
      "CoreML",
    ],
    "libraries": [
      "sqlite3",
    ],
  ],
  "backend_mps": [
    "sha256": "f3f2ce05bb9bb479104a9fb1184af98f120dc729ce290c6e4821680490409db0",
    "sha256" + debug_suffix: "0c1ba0ca0aeb3f720e2a9210e61dee97bbd85832b130863ff825421d65a4544a",
    "frameworks": [
      "Metal",
      "MetalPerformanceShaders",
      "MetalPerformanceShadersGraph",
    ],
  ],
  "backend_xnnpack": [
    "sha256": "3679cac6633f26862c3e114570b900f01ab96495aef006f316e6045bf7863183",
    "sha256" + debug_suffix: "2deee3ffcd25494693e60570ef9f9ce18c93caa358ff434f01024424f0947306",
    "targets": [
      "threadpool",
    ],
  ],
  "executorch": [
    "sha256": "859de39bceabfb228b541d87443a6cc9702f2ec079caa3ad9503aa551e742dde",
    "sha256" + debug_suffix: "31ec5ba126a7cda2d2df7a196454cc1d128c634a140a86d39f06e5e38f6e9a21",
    "libraries": [
      "c++",
    ],
  ],
  "kernels_llm": [
    "sha256": "39726ec3b6f4953e0865b6004d1ee64d84b3631d64a08abce21c1566b8f609a8",
    "sha256" + debug_suffix: "1322523b7f1c5d9ef6d7d7cca78c613a8490e26c6c4b7d0c80caf72023ee5355",
  ],
  "kernels_optimized": [
    "sha256": "ed69ad1bb2f91901a36642b275d026004d173dafbf83a719e10a891a4e315625",
    "sha256" + debug_suffix: "6cd3cc917d1a1ec87c5756759ae0d73a96255e9da7bd7103ded2b2ab16427e46",
    "frameworks": [
      "Accelerate",
    ],
    "targets": [
      "threadpool",
    ],
  ],
  "kernels_quantized": [
    "sha256": "c108099462d8396628730e415592b67a47f530c936f18faa05a02ad2f788ebe4",
    "sha256" + debug_suffix: "bbd914e51098ad25230467332cd377e213089818c703117410206ff8b78557b5",
  ],
])

let targets = deliverables([
  "threadpool": [
    "sha256": "3dadfa574e2a5f8ab01b8e6f732eb6024bbc81d58ca5b582e94a46a78c99a382",
    "sha256" + debug_suffix: "191b57d0e2f8b24712b8aa9fadf9ce22626993aeb23358dfee279e178e509fbb",
  ],
])

let packageProducts: [Product] = products.keys.map { key -> Product in
  .library(name: key, targets: ["\(key)\(dependencies_suffix)"])
}.sorted { $0.name < $1.name }

var packageTargets: [Target] = []

for (key, value) in targets {
  packageTargets.append(.binaryTarget(
    name: key,
    url: "\(url)\(key)-\(version).zip",
    checksum: value["sha256"] as? String ?? ""
  ))
}

for (key, value) in products {
  packageTargets.append(.binaryTarget(
    name: key,
    url: "\(url)\(key)-\(version).zip",
    checksum: value["sha256"] as? String ?? ""
  ))
  let target: Target = .target(
    name: "\(key)\(dependencies_suffix)",
    dependencies: ([key] + (value["targets"] as? [String] ?? []).map {
      key.hasSuffix(debug_suffix) ? $0 + debug_suffix : $0
    }).map { .target(name: $0) },
    path: ".Package.swift/\(key)",
    linkerSettings:
      (value["frameworks"] as? [String] ?? []).map { .linkedFramework($0) } +
      (value["libraries"] as? [String] ?? []).map { .linkedLibrary($0) }
  )
  packageTargets.append(target)
}

let package = Package(
  name: "executorch",
  platforms: [
    .iOS(.v17),
    .macOS(.v12),
  ],
  products: packageProducts,
  targets: packageTargets
)
