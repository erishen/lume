#!/bin/bash
# 预下载 Ghidra 12.1.4 fetchDependencies 全部依赖到 dependencies/downloads/
# （带 sha256 校验 + 重试循环，避开 gradle JVM 下载经代理损坏的问题）。
# 下载列表取自 gradle/support/fetchDependencies.gradle（deps 数组）。
set -u
DL=/src/ghidra/dependencies/downloads
mkdir -p "$DL"
PROXY="http://host.docker.internal:7897"

download() {
  local name="$1" url="$2" sha="$3"
  for attempt in 1 2 3 4 5; do
    if [ -f "$DL/$name" ]; then
      local cur
      cur=$(shasum -a 256 "$DL/$name" 2>/dev/null | cut -d' ' -f1)
      if [ "$cur" = "$sha" ]; then echo "OK(cache) $name"; return 0; fi
      echo "sha不符(重试$attempt) $name: $cur"
      rm -f "$DL/$name"
    fi
    curl -sL --max-time 300 --retry 3 --retry-delay 2 -x "$PROXY" -o "$DL/$name" "$url" || { echo "curl失败(重试$attempt) $name"; sleep 3; continue; }
    local got
    got=$(shasum -a 256 "$DL/$name" 2>/dev/null | cut -d' ' -f1)
    if [ "$got" = "$sha" ]; then echo "OK $name"; return 0; fi
    echo "sha不符(重试$attempt) $name: $got"
    rm -f "$DL/$name"
    sleep 3
  done
  echo "FAILED $name"
  return 1
}

download "java-sarif-2.1-modified.jar" "https://github.com/NationalSecurityAgency/ghidra-data/raw/Ghidra_12.1.4/lib/java-sarif-2.1-modified.jar" "7f736566494756d271aa5e4b1af6c89dc50d074ab1c6374a47df822264226b01"
download "dbgmodel.tlb" "https://github.com/NationalSecurityAgency/ghidra-data/raw/Ghidra_12.1.4/Debugger/dbgmodel.tlb" "8cf5f3a2eb81160aa349056a5ca4c2c726b1b6e98bf3097cd4135147163343c7"
download "AXMLPrinter2.jar" "https://storage.googleapis.com/google-code-archive-downloads/v2/code.google.com/android4me/AXMLPrinter2.jar" "00ed038eb6abaf6ddec8d202a3ed7a81b521458f4cd459948115cfd02ff59d6d"
download "yajsw-stable-13.18.zip" "https://sourceforge.net/projects/yajsw/files/yajsw/yajsw-stable-13.18/yajsw-stable-13.18.zip" "50b34e9817c54b2970fa511ff4584f6556c179f69833aa8b9b1ff6485b61684f"
download "postgresql-15.18.tar.gz" "https://ftp.postgresql.org/pub/source/v15.18/postgresql-15.18.tar.gz" "d32a533516edc688ebbb96ae221b605be2712edd94d7428b42a4af5644250943"
download "PyDev 9.3.0.zip" "https://sourceforge.net/projects/pydev/files/pydev/PyDev%209.3.0/PyDev%209.3.0.zip" "45398edf2adb56078a80bc88a919941578f0c0b363efbdd011bfd158a99b112e"
download "cdt-8.6.0.zip" "https://archive.eclipse.org/tools/cdt/releases/8.6/cdt-8.6.0.zip" "81b7d19d57c4a3009f4761699a72e8d642b5e1d9251d2bb98df438b1e28f8ba9"
download "vs2012_x64.fidb" "https://github.com/NationalSecurityAgency/ghidra-data/raw/Ghidra_12.1.4/FunctionID/vs2012_x64.fidb" "d4e98ab3f790b831793218430bba0d8b24a5fbf4da65b0c1ffa8cb0cfbeb0cdc"
download "vs2012_x86.fidb" "https://github.com/NationalSecurityAgency/ghidra-data/raw/Ghidra_12.1.4/FunctionID/vs2012_x86.fidb" "a490ed7e2ed21e587459feaeace7036b7ede4ce84e72e10dfd8c57434a6918b6"
download "vs2015_x64.fidb" "https://github.com/NationalSecurityAgency/ghidra-data/raw/Ghidra_12.1.4/FunctionID/vs2015_x64.fidb" "e04e9e40f9ecb601c85f4d84ed9bf66b45363be1d1e82c162e4c9902b8cb508f"
download "vs2015_x86.fidb" "https://github.com/NationalSecurityAgency/ghidra-data/raw/Ghidra_12.1.4/FunctionID/vs2015_x86.fidb" "b66ee696653e2ed365919deaaef885103120c792e22e79af70d1209d7e1d8644"
download "vs2017_x64.fidb" "https://github.com/NationalSecurityAgency/ghidra-data/raw/Ghidra_12.1.4/FunctionID/vs2017_x64.fidb" "d5fa5f697298174fa53d247d3599e6a12884605ad181c7b954e2380ec1f0bd89"
download "vs2017_x86.fidb" "https://github.com/NationalSecurityAgency/ghidra-data/raw/Ghidra_12.1.4/FunctionID/vs2017_x86.fidb" "d389cb8d76ff4a59ca35f891b8521c72ad5f0df96e253973a2d21a8614a4cc81"
download "vs2019_x64.fidb" "https://github.com/NationalSecurityAgency/ghidra-data/raw/Ghidra_12.1.4/FunctionID/vs2019_x64.fidb" "150007796fc36a4069660ad62449aadaaf3dd11b3864a5ef21e79831c9ce9118"
download "vs2019_x86.fidb" "https://github.com/NationalSecurityAgency/ghidra-data/raw/Ghidra_12.1.4/FunctionID/vs2019_x86.fidb" "eb630a36faa586a371eb734dc0bbd8d13ccaef697f3db5872596358f3dd2432a"
download "vsOlder_x64.fidb" "https://github.com/NationalSecurityAgency/ghidra-data/raw/Ghidra_12.1.4/FunctionID/vsOlder_x64.fidb" "8c3b51f4660cd27e1a0d610a9f3f2d5fbc833a66ac9ee4393ee2f2481e855866"
download "vsOlder_x86.fidb" "https://github.com/NationalSecurityAgency/ghidra-data/raw/Ghidra_12.1.4/FunctionID/vsOlder_x86.fidb" "98605c6b6b9214a945d844e41c85860d54649a45bca7873ef6991c0e93720787"
download "z3-4.13.0-x64-glibc-2.31.zip" "https://github.com/Z3Prover/z3/releases/download/z3-4.13.0/z3-4.13.0-x64-glibc-2.31.zip" "bc31ad12446d7db1bd9d0ac82dec9d7b5129b8b8dd6e44b571a83ac6010d2f9b"
download "z3-4.13.0-arm64-osx-11.0.zip" "https://github.com/Z3Prover/z3/releases/download/z3-4.13.0/z3-4.13.0-arm64-osx-11.0.zip" "e7cd325cb2210d3b241d0d5517a293677030f58c1771e196c4574ef99dc45168"
download "z3-4.13.0-x64-osx-11.7.10.zip" "https://github.com/Z3Prover/z3/releases/download/z3-4.13.0/z3-4.13.0-x64-osx-11.7.10.zip" "0c33d8574f7dcd041f1f4e7fe301840db6a527f866cb74b0b47518bf8053502d"
download "z3-4.13.0-x64-win.zip" "https://github.com/Z3Prover/z3/releases/download/z3-4.13.0/z3-4.13.0-x64-win.zip" "2bcd14b6849fddead3d0b3cb671cc9d134a8ef0b1d3eff3efd7d75a5bbf00dd3"
download "protobuf-6.31.0-py3-none-any.whl" "https://files.pythonhosted.org/packages/ee/01/1ed1d482960a5718fd99c82f6d79120181947cfd4667ec3944d448ed44a3/protobuf-6.31.0-py3-none-any.whl" "6ac2e82556e822c17a8d23aa1190bbc1d06efb9c261981da95c71c9da09e9e23"
download "psutil-5.9.8.tar.gz" "https://files.pythonhosted.org/packages/90/c7/6dc0a455d111f68ee43f27793971cf03fe29b6ef972042549db29eec39a2/psutil-5.9.8.tar.gz" "6be126e3225486dff286a8fb9a06246a5253f4c7c53b475ea5f5ac934e64194c"
download "setuptools-80.9.0-py3-none-any.whl" "https://files.pythonhosted.org/packages/a3/dc/17031897dae0efacfea57dfd3a82fdd2a2aeb58e0ff71b77b87e44edc772/setuptools-80.9.0-py3-none-any.whl" "062d34222ad13e0cc312a4c02d73f059e86a4acbfbdea8f8f76b28c99f306922"
download "wheel-0.45.1-py3-none-any.whl" "https://files.pythonhosted.org/packages/0b/2c/87f3254fd8ffd29e4c02732eee68a83a1d3c346ae39bc6822dcbcb697f2b/wheel-0.45.1-py3-none-any.whl" "708e7481cc80179af0e556bbf0cc00b8444c7321e2700b8d8580231d13017248"
download "pybag-2.2.16-py3-none-any.whl" "https://files.pythonhosted.org/packages/5e/a2/52084698c0a3c7e7a94ddfff26f83df09a50781b8436f6f203f5de0a457c/pybag-2.2.16-py3-none-any.whl" "cef631e87d0bb12163c8cb11ca756ddfa8924a6eacdbd746683ed58948a2bfdf"
download "capstone-5.0.6-py3-none-win_amd64.whl" "https://files.pythonhosted.org/packages/da/72/ff7894c2fb5716d9a3ce9c27ba34b29d991a11d8442d2ef0fcdc5564ba7e/capstone-5.0.6-py3-none-win_amd64.whl" "761c3deae00b22ac697081cdae1383bb90659dd0d79387a09cf5bdbb22b17064"
download "comtypes-1.4.13-py3-none-any.whl" "https://files.pythonhosted.org/packages/66/95/f30c80615fda0d3c0ee6493ac9db61183313b43499b62dec136773b0e870/comtypes-1.4.13-py3-none-any.whl" "21546210748ba52e839e52112124b16ffab7d7fb68096493165fbc249e9023ad"
download "pywin32-311-cp314-cp314-win_amd64.whl" "https://files.pythonhosted.org/packages/90/4b/07c77d8ba0e01349358082713400435347df8426208171ce297da32c313d/pywin32-311-cp314-cp314-win_amd64.whl" "3aca44c046bd2ed8c90de9cb8427f581c479e594e99b5c0bb19b29c10fd6cb87"
download "pywin32-311-cp313-cp313-win_amd64.whl" "https://files.pythonhosted.org/packages/e3/28/e0a1909523c6890208295a29e05c2adb2126364e289826c0a8bc7297bd5c/pywin32-311-cp313-cp313-win_amd64.whl" "718a38f7e5b058e76aee1c56ddd06908116d35147e133427e59a3983f703a20d"
download "pywin32-311-cp312-cp312-win_amd64.whl" "https://files.pythonhosted.org/packages/d1/a8/a0e8d07d4d051ec7502cd58b291ec98dcc0c3fff027caad0470b72cfcc2f/pywin32-311-cp312-cp312-win_amd64.whl" "b8c095edad5c211ff31c05223658e71bf7116daa0ecf3ad85f3201ea3190d067"
download "pywin32-311-cp311-cp311-win_amd64.whl" "https://files.pythonhosted.org/packages/51/8f/9bb81dd5bb77d22243d33c8397f09377056d5c687aa6d4042bea7fbf8364/pywin32-311-cp311-cp311-win_amd64.whl" "3ce80b34b22b17ccbd937a6e78e7225d80c52f5ab9940fe0506a1a16f3dab503"
download "pywin32-311-cp310-cp310-win_amd64.whl" "https://files.pythonhosted.org/packages/5e/bf/360243b1e953bd254a82f12653974be395ba880e7ec23e3731d9f73921cc/pywin32-311-cp310-cp310-win_amd64.whl" "797c2772017851984b97180b0bebe4b620bb86328e8a884bb626156295a63b3b"
download "pywin32-311-cp39-cp39-win_amd64.whl" "https://files.pythonhosted.org/packages/9f/8a/1403d0353f8c5a2f0829d2b1c4becbf9da2f0a4d040886404fc4a5431e4d/pywin32-311-cp39-cp39-win_amd64.whl" "e0c4cfb0621281fe40387df582097fd796e80430597cb9944f0ae70447bacd91"
download "win32more-0.7.0-py3-none-any.whl" "https://files.pythonhosted.org/packages/92/3a/658eb3ba88f067662be280f8f1aec07a70c96bac77e9edc48b1be38e446b/win32more-0.7.0-py3-none-any.whl" "ba586b8a5997d1410de5d934e34aefdce8d15ef8d766d6a6b1cda8762d46b05e"
download "win32more_appsdk-0.7.3-py2.py3-none-any.whl" "https://files.pythonhosted.org/packages/f6/49/cff21fc3adcac1fe19f9f2e50278553fe13b9ac702be569c97f85292481d/win32more_appsdk-0.7.3-py2.py3-none-any.whl" "707cdd2db3f07dfd7d22b5aa2adffd13dbd316a5eed779ca272feaa6ebc22f88"
download "win32more_core-0.7.0-py2.py3-none-any.whl" "https://files.pythonhosted.org/packages/81/bc/7c1c5609f835d3e719c944714638a7a70bbfef6b990c251c32c1cd82d67a/win32more_core-0.7.0-py2.py3-none-any.whl" "a501666b44bd36ceebbcff7656e7ec75f92813d23761d0f49f14c79b3a406bd9"
download "win32more_microsoft_graphics_win2d-0.7.1.3.2-py2.py3-none-any.whl" "https://files.pythonhosted.org/packages/b5/6d/d58b223be7cff571527d91a3626089cb4da614dcba461811dcf18449f0f9/win32more_microsoft_graphics_win2d-0.7.1.3.2-py2.py3-none-any.whl" "548f65fcebe72c19ae9c797dd9351c0d0bd20010856a4928f755f6d4ec01b16a"
download "win32more_microsoft_web_webview2-0.7.1.0.3650.58-py2.py3-none-any.whl" "https://files.pythonhosted.org/packages/6c/8e/fdfc9f796aad6b622ff197b3e58c9689db95482dbf7817cd99d351b54719/win32more_microsoft_web_webview2-0.7.1.0.3650.58-py2.py3-none-any.whl" "c11d2f04a0a50a64d0ee60d159ced1a5e16a739e43c578e2cb909f169a39458b"
download "win32more_microsoft_windows_sdk_contracts-0.7.10.0.26100.6901-py2.py3-none-any.whl" "https://files.pythonhosted.org/packages/56/d7/0bb36964811a23f262cdddb1e5b91fdbd2f494990d48e657f15108f04855/win32more_microsoft_windows_sdk_contracts-0.7.10.0.26100.6901-py2.py3-none-any.whl" "392a62a6ec7b0025f1e5d6f55dbfb55191f0088db8f6a2fc769413ab36b2bbcf"
download "win32more_microsoft_windows_sdk_win32metadata-0.7.68.0.4-py2.py3-none-any.whl" "https://files.pythonhosted.org/packages/e5/37/8706eb126fc52e59526304aa89ee30771a894e489aa0ea835f101e499b17/win32more_microsoft_windows_sdk_win32metadata-0.7.68.0.4-py2.py3-none-any.whl" "d9e8ab3a7c4efb1dabb18d1b988c32648f317468da8e89bb649fdb6e1a97aa3f"
download "win32more_microsoft_windowsappsdk-0.7.1.8.251106002-py2.py3-none-any.whl" "https://files.pythonhosted.org/packages/ef/7c/9630708dd252e0c5c7c9975d52ba0deea996e9b0cf29784d521ac30f3a48/win32more_microsoft_windowsappsdk-0.7.1.8.251106002-py2.py3-none-any.whl" "d37c42d0e6ac87f71a4a1209af8a6adadde19d4b6df44ab3375195f5f41c462c"
download "packaging-25.0-py3-none-any.whl" "https://files.pythonhosted.org/packages/20/12/38679034af332785aac8774540895e234f4d07f7545804097de4b666afd8/packaging-25.0-py3-none-any.whl" "29572ef2b1f17581046b3a2227d5c611fb25ec70ca1ba8554b24b0e69331a484"
download "jpype1-1.5.2-cp313-cp313-macosx_10_13_universal2.whl" "https://files.pythonhosted.org/packages/76/be/b37005bec457b94eaaf637a663073b7c5df70113fd4ae4865f6e386c612f/jpype1-1.5.2-cp313-cp313-macosx_10_13_universal2.whl" "4acb098cb1698b14b6e5c79e275f4c70dcc01b0fb93425f206d0a5e380e43c66"
download "jpype1-1.5.2-cp313-cp313-manylinux_2_17_aarch64.manylinux2014_aarch64.whl" "https://files.pythonhosted.org/packages/20/a3/00a265d424f7d47e0dc547df2320225ce0143fec671faf710def41404b8c/jpype1-1.5.2-cp313-cp313-manylinux_2_17_aarch64.manylinux2014_aarch64.whl" "c08480c7d18125664a12bf0a244b96b49c05105306b65937dbefeb05ab4b2847"
download "jpype1-1.5.2-cp313-cp313-manylinux_2_17_x86_64.manylinux2014_x86_64.whl" "https://files.pythonhosted.org/packages/6d/d0/191db2e9ab6ae7029368a488c9d88235966843b185aba7925e54aa0c0013/jpype1-1.5.2-cp313-cp313-manylinux_2_17_x86_64.manylinux2014_x86_64.whl" "42fe8db66ad4e5c66f637f5c4de82fca880ba696104e1f4a7e575885923dead8"
download "jpype1-1.5.2-cp313-cp313-win_amd64.whl" "https://files.pythonhosted.org/packages/e3/b7/e1787633b41d609320b41d0dd87fe3118598210609e4e3f6cef93cfcef40/jpype1-1.5.2-cp313-cp313-win_amd64.whl" "2b96365f1302df2fb3c6ad73117d6fe450a55b7550fd7fecadac3cec5bc7117c"
download "jpype1-1.5.2-cp312-cp312-macosx_10_9_universal2.whl" "https://files.pythonhosted.org/packages/8d/e4/0c27352e8222dcc0e3ce44b298015072d2057d08dd353541c980a31d26c9/jpype1-1.5.2-cp312-cp312-macosx_10_9_universal2.whl" "1e1db9ac909ad2ae0e40b04c2aa88cb14250d5245d69715561507681f2b08b2f"
download "jpype1-1.5.2-cp312-cp312-manylinux_2_17_aarch64.manylinux2014_aarch64.whl" "https://files.pythonhosted.org/packages/fa/4c/e0200a6e3fed5cda79e926c2a8a610676f04948f89d7e38d93c7d4b21be9/jpype1-1.5.2-cp312-cp312-manylinux_2_17_aarch64.manylinux2014_aarch64.whl" "994fb7b319b453f77ad4b6aff01e0dd4180ea74a6fe5a031e4e9db92dbe95376"
download "jpype1-1.5.2-cp312-cp312-manylinux_2_17_x86_64.manylinux2014_x86_64.whl" "https://files.pythonhosted.org/packages/74/f3/1cd4332076ed0421e703412f47f15f43af170809435c57ba3162edc80d4b/jpype1-1.5.2-cp312-cp312-manylinux_2_17_x86_64.manylinux2014_x86_64.whl" "b5b1fb2b430a50f081ea0ee24d19232ae0d03dbfe3dd076ec5f8ae42b30a656f"
download "jpype1-1.5.2-cp312-cp312-win_amd64.whl" "https://files.pythonhosted.org/packages/74/dd/7408d4beae755de6fcd07c76b2f0bacabc0461b43fba83811c1f7c22440e/jpype1-1.5.2-cp312-cp312-win_amd64.whl" "c7b1c2d76d211cab60be16505d32a6b3c9fffc51ce79c68e81a3d48e5effff2d"
download "jpype1-1.5.2-cp311-cp311-macosx_10_9_universal2.whl" "https://files.pythonhosted.org/packages/35/a0/638186a75026a02286041e4a0449b1dff799a3914dc1c0716ef9b9367b73/jpype1-1.5.2-cp311-cp311-macosx_10_9_universal2.whl" "c9f6ab8dd284c16e2617a697d54c3d0304b08020a37386ed96103a129391a2d9"
download "jpype1-1.5.2-cp311-cp311-manylinux_2_17_aarch64.manylinux2014_aarch64.whl" "https://files.pythonhosted.org/packages/0e/78/95db2eb3c8a7311ee08a2c237cea24828859db6a6cb5e901971d3f5e49da/jpype1-1.5.2-cp311-cp311-manylinux_2_17_aarch64.manylinux2014_aarch64.whl" "a54a771ee56260f98e5b9a77455084e4a48061967de13dabf628bdba9c8122e0"
download "jpype1-1.5.2-cp311-cp311-manylinux_2_17_x86_64.manylinux2014_x86_64.whl" "https://files.pythonhosted.org/packages/0b/7d/9fdbbc1a574be43f9820735ca8df0caf8b159856201d9b21fd73932342bc/jpype1-1.5.2-cp311-cp311-manylinux_2_17_x86_64.manylinux2014_x86_64.whl" "b900e154826a076118d074166596f1d817e113e07084bf0c9c43d8064a86ab77"
download "jpype1-1.5.2-cp311-cp311-win_amd64.whl" "https://files.pythonhosted.org/packages/0e/b9/4dfb38a7f4efb21f71df7344944a8d9a23e30d0503574e455af6ce4f1a56/jpype1-1.5.2-cp311-cp311-win_amd64.whl" "0a0d18d4384b3df2e55282545737dfcf18c604504f1382ad14f880bef960f265"
download "jpype1-1.5.2-cp310-cp310-macosx_10_9_universal2.whl" "https://files.pythonhosted.org/packages/c7/f2/b2efcad1ea5a541f125218e4eb1529ebb8ca18941264c879f3e89a36dc35/jpype1-1.5.2-cp310-cp310-macosx_10_9_universal2.whl" "7b2da98c142812ca40a18a735b33e47c6511b03debf1e979630f4cf473b68a87"
download "jpype1-1.5.2-cp310-cp310-manylinux_2_17_aarch64.manylinux2014_aarch64.whl" "https://files.pythonhosted.org/packages/c0/c6/63538d160c17e837f62d29ba4163bc444cef08c29cd3f3b8090691c1869c/jpype1-1.5.2-cp310-cp310-manylinux_2_17_aarch64.manylinux2014_aarch64.whl" "fcfc5c1d45d6b108800d172ea817bda585db7f1646d6a98d14da9aca66e0eb44"
download "jpype1-1.5.2-cp310-cp310-manylinux_2_17_x86_64.manylinux2014_x86_64.whl" "https://files.pythonhosted.org/packages/97/0a/cbe03759331c640aa5862f974028122a862b08935a0b11b8fa6f6e46c26b/jpype1-1.5.2-cp310-cp310-manylinux_2_17_x86_64.manylinux2014_x86_64.whl" "cdca93cc74f8db1f604d2ea6adb764dec4dec68528f1ee68308fa3d524095739"
download "jpype1-1.5.2-cp310-cp310-win_amd64.whl" "https://files.pythonhosted.org/packages/22/18/0a51845ca890ffdc72f4d71a0c2be334b887c5bb6812207efe5ad45afcb3/jpype1-1.5.2-cp310-cp310-win_amd64.whl" "924b0a0cf93d3dddb3f79286fbe40f8c901c78ed61216edbe108666234df43e0"
download "jpype1-1.5.2-cp39-cp39-macosx_10_9_x86_64.whl" "https://files.pythonhosted.org/packages/05/71/590b2a91b43763aa27eac2c63803542a2878a4d8c600b81aa694d3fde919/jpype1-1.5.2-cp39-cp39-macosx_10_9_x86_64.whl" "8b75d33e93a3bc6543ddf97c24ee0adb5a86a69fb67f0e4f4fa1c8c3970bbf98"
download "jpype1-1.5.2-cp39-cp39-manylinux_2_17_aarch64.manylinux2014_aarch64.whl" "https://files.pythonhosted.org/packages/77/6b/130fb6d0c43976b4e129c6bc19daf0e25c42fc38c5096ed92c4105bfd2c4/jpype1-1.5.2-cp39-cp39-manylinux_2_17_aarch64.manylinux2014_aarch64.whl" "ea21bca4cece752cd3ee88fcd62ce8f444feac8dc7244475fdb9c0e8712e07ea"
download "jpype1-1.5.2-cp39-cp39-manylinux_2_17_x86_64.manylinux2014_x86_64.whl" "https://files.pythonhosted.org/packages/77/91/f08a719461a390b48d9096b50f1f4a49ee281007ec192e51073090d3d8b7/jpype1-1.5.2-cp39-cp39-manylinux_2_17_x86_64.manylinux2014_x86_64.whl" "54744265ef36665d110d139a4b81d10532694c6077b23ef60f3609feadc22d30"
download "jpype1-1.5.2-cp39-cp39-win_amd64.whl" "https://files.pythonhosted.org/packages/e5/cf/344e1f81f1e8c651ec23dfa9fe4b91f6e1d699b36f610a547ba85ee7fb16/jpype1-1.5.2-cp39-cp39-win_amd64.whl" "68e1d118200fc46f4ea4bf20900081587ec04de484037c997b0a3b7c5eb71fe3"
download "jpype1-1.5.2.tar.gz" "https://files.pythonhosted.org/packages/bd/68/47fa634cbd0418cbca86355e9421425f5892ee994f7338106327e49f9117/jpype1-1.5.2.tar.gz" "74a42eccf21d30394c1832aec3985a14965fa5320da087b65029d172c0cec43b"

echo "=== 全部依赖预下载完成 ==="
