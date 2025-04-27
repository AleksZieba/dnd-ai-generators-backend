# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/mnt/c/Users/jfox1/workspace/github.com/AleksZieba/dnd-gear-generator-gemini-backend-cpp/build/_deps/crow-src"
  "/mnt/c/Users/jfox1/workspace/github.com/AleksZieba/dnd-gear-generator-gemini-backend-cpp/build/_deps/crow-build"
  "/mnt/c/Users/jfox1/workspace/github.com/AleksZieba/dnd-gear-generator-gemini-backend-cpp/build/_deps/crow-subbuild/crow-populate-prefix"
  "/mnt/c/Users/jfox1/workspace/github.com/AleksZieba/dnd-gear-generator-gemini-backend-cpp/build/_deps/crow-subbuild/crow-populate-prefix/tmp"
  "/mnt/c/Users/jfox1/workspace/github.com/AleksZieba/dnd-gear-generator-gemini-backend-cpp/build/_deps/crow-subbuild/crow-populate-prefix/src/crow-populate-stamp"
  "/mnt/c/Users/jfox1/workspace/github.com/AleksZieba/dnd-gear-generator-gemini-backend-cpp/build/_deps/crow-subbuild/crow-populate-prefix/src"
  "/mnt/c/Users/jfox1/workspace/github.com/AleksZieba/dnd-gear-generator-gemini-backend-cpp/build/_deps/crow-subbuild/crow-populate-prefix/src/crow-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/mnt/c/Users/jfox1/workspace/github.com/AleksZieba/dnd-gear-generator-gemini-backend-cpp/build/_deps/crow-subbuild/crow-populate-prefix/src/crow-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/mnt/c/Users/jfox1/workspace/github.com/AleksZieba/dnd-gear-generator-gemini-backend-cpp/build/_deps/crow-subbuild/crow-populate-prefix/src/crow-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
