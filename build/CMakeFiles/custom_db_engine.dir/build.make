# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.28

# Delete rule output on recipe failure.
.DELETE_ON_ERROR:

#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:

# Disable VCS-based implicit rules.
% : %,v

# Disable VCS-based implicit rules.
% : RCS/%

# Disable VCS-based implicit rules.
% : RCS/%,v

# Disable VCS-based implicit rules.
% : SCCS/s.%

# Disable VCS-based implicit rules.
% : s.%

.SUFFIXES: .hpux_make_needs_suffix_list

# Command-line flag to silence nested $(MAKE).
$(VERBOSE)MAKESILENT = -s

#Suppress display of executed commands.
$(VERBOSE).SILENT:

# A target that is always out of date.
cmake_force:
.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /usr/bin/cmake

# The command to remove a file.
RM = /usr/bin/cmake -E rm -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /home/bml/Desktop/custom_db_engine

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /home/bml/Desktop/custom_db_engine/build

# Include any dependencies generated for this target.
include CMakeFiles/custom_db_engine.dir/depend.make
# Include any dependencies generated by the compiler for this target.
include CMakeFiles/custom_db_engine.dir/compiler_depend.make

# Include the progress variables for this target.
include CMakeFiles/custom_db_engine.dir/progress.make

# Include the compile flags for this target's objects.
include CMakeFiles/custom_db_engine.dir/flags.make

CMakeFiles/custom_db_engine.dir/src/storage_engine.cpp.o: CMakeFiles/custom_db_engine.dir/flags.make
CMakeFiles/custom_db_engine.dir/src/storage_engine.cpp.o: /home/bml/Desktop/custom_db_engine/src/storage_engine.cpp
CMakeFiles/custom_db_engine.dir/src/storage_engine.cpp.o: CMakeFiles/custom_db_engine.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color "--switch=$(COLOR)" --green --progress-dir=/home/bml/Desktop/custom_db_engine/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building CXX object CMakeFiles/custom_db_engine.dir/src/storage_engine.cpp.o"
	/usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -MD -MT CMakeFiles/custom_db_engine.dir/src/storage_engine.cpp.o -MF CMakeFiles/custom_db_engine.dir/src/storage_engine.cpp.o.d -o CMakeFiles/custom_db_engine.dir/src/storage_engine.cpp.o -c /home/bml/Desktop/custom_db_engine/src/storage_engine.cpp

CMakeFiles/custom_db_engine.dir/src/storage_engine.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color "--switch=$(COLOR)" --green "Preprocessing CXX source to CMakeFiles/custom_db_engine.dir/src/storage_engine.cpp.i"
	/usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /home/bml/Desktop/custom_db_engine/src/storage_engine.cpp > CMakeFiles/custom_db_engine.dir/src/storage_engine.cpp.i

CMakeFiles/custom_db_engine.dir/src/storage_engine.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color "--switch=$(COLOR)" --green "Compiling CXX source to assembly CMakeFiles/custom_db_engine.dir/src/storage_engine.cpp.s"
	/usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /home/bml/Desktop/custom_db_engine/src/storage_engine.cpp -o CMakeFiles/custom_db_engine.dir/src/storage_engine.cpp.s

# Object files for target custom_db_engine
custom_db_engine_OBJECTS = \
"CMakeFiles/custom_db_engine.dir/src/storage_engine.cpp.o"

# External object files for target custom_db_engine
custom_db_engine_EXTERNAL_OBJECTS =

custom_db_engine: CMakeFiles/custom_db_engine.dir/src/storage_engine.cpp.o
custom_db_engine: CMakeFiles/custom_db_engine.dir/build.make
custom_db_engine: CMakeFiles/custom_db_engine.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color "--switch=$(COLOR)" --green --bold --progress-dir=/home/bml/Desktop/custom_db_engine/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Linking CXX executable custom_db_engine"
	$(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/custom_db_engine.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
CMakeFiles/custom_db_engine.dir/build: custom_db_engine
.PHONY : CMakeFiles/custom_db_engine.dir/build

CMakeFiles/custom_db_engine.dir/clean:
	$(CMAKE_COMMAND) -P CMakeFiles/custom_db_engine.dir/cmake_clean.cmake
.PHONY : CMakeFiles/custom_db_engine.dir/clean

CMakeFiles/custom_db_engine.dir/depend:
	cd /home/bml/Desktop/custom_db_engine/build && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /home/bml/Desktop/custom_db_engine /home/bml/Desktop/custom_db_engine /home/bml/Desktop/custom_db_engine/build /home/bml/Desktop/custom_db_engine/build /home/bml/Desktop/custom_db_engine/build/CMakeFiles/custom_db_engine.dir/DependInfo.cmake "--color=$(COLOR)"
.PHONY : CMakeFiles/custom_db_engine.dir/depend

