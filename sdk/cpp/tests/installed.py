"""Compile/link/run using only the installed public SDK and its CMake target."""
import pathlib, subprocess, sys, tempfile
with tempfile.TemporaryDirectory() as directory:
    root=pathlib.Path(directory);prefix=root/'prefix';source=root/'consumer';source.mkdir()
    subprocess.run(['cmake','--install',sys.argv[1],'--prefix',str(prefix)],check=True)
    (source/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.25)\nproject(consumer LANGUAGES CXX)\nfind_package(nuvo_depth_client CONFIG REQUIRED)\nadd_executable(consumer main.cpp)\ntarget_link_libraries(consumer PRIVATE nuvo::client)\n')
    (source/'main.cpp').write_text('#include <nuvo/client.hpp>\nint main(){nuvo::client::validate({});return 0;}\n')
    subprocess.run(['cmake','-S',str(source),'-B',str(root/'build'),'-DCMAKE_PREFIX_PATH='+str(prefix)],check=True)
    subprocess.run(['cmake','--build',str(root/'build')],check=True)
    subprocess.run([str(root/'build/consumer')],check=True)
