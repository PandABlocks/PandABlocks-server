Changelog
=========

All notable changes to this project will be documented in this file.

The format is based on `Keep a Changelog
<https://keepachangelog.com/en/1.0.0/>`_, and this project adheres to `Semantic
Versioning <https://semver.org/spec/v2.0.0.html>`_.

Unreleased
----------

- Change to extension server: parsing now returns callables, and add
  ExtensionHelper to assist in building extensions
- New attribute to interrogate number of words in a table row
- UNSCALED capture is no longer supported (issue #24)
- Allow .CAPTURE to take arbitrary lists of options (issue #23) and add
  supporting methods for interrogating available options
- Add support for Standard Deviation capture if supported by FPGA
- Fix error in scalar value formatting (github issue #29)
- Add ``*BITS?`` command to return available bit bus entry names

3.0a1_ - 2021-09-21
-------------------

- Added Continuous Integration
- Support building on 64-bit ARM
- Update default toolchain.  This will also require a PandA system update
- Add timestamp of time of arming capture
- Fix page fault when reading capture completion with recent kernel

2.1_ - 2021-04-27
-------------------

- Start keeping a changelog
- Implement and document changes to extension server implementing github issue
  #9


.. _Unreleased: https://github.com/PandABlocks/PandABlocks-FPGA
.. _3.0a1: ../../compare/2.1...3.0a1
.. _2.1: ../../releases/tag/2.1
