# Helper function to set NPB compile definitions Used by both LCI and MPI
# targets class: the class letter (A, B, C, D, or E)
function(set_npb_definitions target class)
  string(TOUPPER "${class}" class_upper)
  string(REPLACE ";" " " mpi_libraries "${MPI_C_LIBRARIES}")
  string(REPLACE ";" " " mpi_include_dirs "${MPI_C_INCLUDE_DIRS}")
  target_compile_definitions(
    ${target}
    PRIVATE CLASS='${class_upper}'
            NPBVERSION="3.4.3"
            COMPILETIME="${CMAKE_CURRENT_SOURCE_DIR}"
            MPICC="${MPI_C_COMPILER}"
            CFLAGS="${CMAKE_C_FLAGS}"
            CLINK="${MPI_C_COMPILER}"
            CLINKFLAGS="${CMAKE_C_FLAGS}"
            CMPI_LIB="${mpi_libraries}"
            CMPI_INC="${mpi_include_dirs}")
endfunction()
