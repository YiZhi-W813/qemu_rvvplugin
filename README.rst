===========
QEMU_RVVPLUGIN README
===========
修改了hotblocks插件，支持统计更多与rvv相关的信息，包括每个tb的rvv指令数/vls指令数、执行的指令计数、执行的向量指令计数、执行的向量指令trace，并在rvvplugin_utils目录下提供了一个py脚本用来聚类各rvv指令的执行计数。
用法包括但不限于通过excel分析hotblock log来得到最热的向量代码指令段，然后addr2line从指令地址反向分析源码行为；聚类得到被执行的次数较多的向量指令类型等等

Building
========


.. code-block:: shell

  mkdir build
  cd build
  ../configure --enable-plugins 
  make -j64

Example
==================

omnetpp (only an example):

.. code-block:: shell

    ./build/qemu-riscv64 -plugin ./build/contrib/plugins/libhotblocks.so \
    -d plugin \
    471.omnetpp(your elf) \
    -f omnetpp.ini(your input) \
    2> ./rvvplugin_utils/plugin_rpt.txt 3> ./rvvplugin_utils/plugin_vinstrace.txt \

    cd rvvplugin_utils
    python ./classify_v_instructions.py