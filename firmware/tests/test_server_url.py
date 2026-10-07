#!/usr/bin/env python3
"""Real bounded server URL policy, no hardware or credentials."""
import os, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[2]
source=r'''
#include <cassert>
#include "network/server_url.h"
int main() {
 using network::validServerUrl;
 assert(validServerUrl("http://192.168.1.20:8000", "192.168.1.4"));
 assert(validServerUrl("https://10.0.0.8", "192.168.1.4"));
 for (auto s : {"", "http://localhost:8000", "http://127.1.2.3", "http://0.0.0.0", "http://224.0.0.1", "http://255.255.255.255", "http://169.254.1.1", "http://192.168.1.4:8000", "http://user@192.168.1.20", "http://192.168.1.20/path", "http://192.168.1.20/", "http://192.168.1.20?x", "http://192.168.1.20#x", "ftp://192.168.1.20", "http://192.168.1.20:0", "http://192.168.1.20:65536", "http://192.168.1.20:080", "http://192.168.01.20", "http://192.168.1.20\n", "http://192.168.1.256", "http://192.168.1.20:80junk"}) assert(!validServerUrl(s,"192.168.1.4"));
 assert(!validServerUrl(std::string(200,'a'), ""));
}
'''
with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as d:
 p=Path(d); (p/'test.cpp').write_text(source)
 subprocess.run([os.environ.get('CXX','g++'),'-std=c++17',*os.environ.get('HOST_TEST_FLAGS','').split(),'-I'+str(root/'firmware/src'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS bounded IPv4 HTTP/HTTPS server URL validation')
