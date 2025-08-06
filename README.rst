===========
QEMU_RVVPLUGIN README
===========
修改了hotblocks插件，支持统计更多与rvv相关的信息，包括每个tb的rvv指令数/vls指令数、执行的指令计数、执行的向量指令计数、执行的向量指令trace，并在rvvplugin_utils目录下提供了一个py脚本用来聚类各rvv指令的执行计数。

Building
========


.. code-block:: shell

  mkdir build
  cd build
  ../configure --enable-plugins 
  make -j64

Example
==================
omnetpp(only an example):

.. code-block:: shell
  ./build/qemu-riscv64 -plugin /nfs/home/zhaozhi/workspace/qemu/build/contrib/plugins/libhotblocks.so \
  -d plugin /nfs/home/zhaozhi/workspace/omnetpp_buildres/raw_v/471.omnetpp \
  -f /nfs/home/zhaozhi/workspace/rvv-compare/vector/cpu2006v99/benchspec/CPU2006/471.omnetpp/data/test/input/omnetpp.ini \
  2> ./rvvplugin_utils/plugin_rpt.txt 3> ./rvvplugin_utils/plugin_vinstrace.txt \

  cd rvvplugin_utils
  python ./classify_v_instructions.py