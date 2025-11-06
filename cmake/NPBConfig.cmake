# Helper function to set NPB compile definitions Used by both LCI and MPI
# targets class: the class letter (A, B, C, D, or E)
function(set_npb_definitions target class)
  string(TOUPPER "${class}" class_upper)
  target_compile_definitions(
    ${target}
    PRIVATE CLASS='${class_upper}'
            NPBVERSION="3.4.3"
            COMPILETIME="${CMAKE_CURRENT_SOURCE_DIR}"
            MPICC="${MPI_C_COMPILER}"
            CFLAGS="${CMAKE_C_FLAGS}"
            CLINK="$(MPICC)"
            CLINKFLAGS="$(CFLAGS)"
            CMPI_LIB="-L/usr/local/lib -lmpi -lpmpi"
            CMPI_INC="-I/usr/local/include")
endfunction()
