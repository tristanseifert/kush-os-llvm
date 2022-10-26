//===--- Kush.cpp - Kush ToolChain Implementations --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "Kush.h"
#include "CommonArgs.h"
#include "clang/Config/config.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Options.h"
#include "clang/Driver/SanitizerArgs.h"
#include "llvm/Option/ArgList.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

/**
 * Construct a linker invocation.
 */
void kush::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                   const InputInfo &Output,
                                   const InputInfoList &Inputs,
                                   const ArgList &Args,
                                   const char *LinkingOutput) const {
  const toolchains::Kush &ToolChain =
      static_cast<const toolchains::Kush &>(getToolChain());
  const Driver &D = ToolChain.getDriver();

  ArgStringList CmdArgs;

  // Silence warning for "clang -g foo.o -o foo"
  Args.ClaimAllArgs(options::OPT_g_Group);
  // and "clang -emit-llvm foo.o -o foo"
  Args.ClaimAllArgs(options::OPT_emit_llvm);
  // and for "clang -w foo.o -o foo". Other warning options are already
  // handled somewhere else.
  Args.ClaimAllArgs(options::OPT_w);

  const char *Exec = Args.MakeArgString(ToolChain.GetLinkerPath());
  /*if (llvm::sys::path::filename(Exec).equals_lower("ld.lld") ||
      llvm::sys::path::stem(Exec).equals_lower("ld.lld")) {
    CmdArgs.push_back("-z");
    CmdArgs.push_back("rodynamic");
    CmdArgs.push_back("-z");
    CmdArgs.push_back("separate-loadable-segments");
    CmdArgs.push_back("--pack-dyn-relocs=relr");
  }*/

  // currently, we don't support protected relro 
  CmdArgs.push_back("-znorelro");

  // specify sysroot
  if (!D.SysRoot.empty())
    CmdArgs.push_back(Args.MakeArgString("--sysroot=" + D.SysRoot));

  const bool IsPIE =
      !Args.hasArg(options::OPT_shared) &&
      (Args.hasArg(options::OPT_pie) || ToolChain.isPIEDefault(Args));

  if (IsPIE)
    CmdArgs.push_back("-pie");

  if (Args.hasArg(options::OPT_s))
    CmdArgs.push_back("-s");

  // static linking flags
  if (Args.hasArg(options::OPT_static)) {
    CmdArgs.push_back("-Bstatic");
  } 
  // dynamic linking flags
  else {
    if (Args.hasArg(options::OPT_rdynamic))
      CmdArgs.push_back("-export-dynamic");
    if (Args.hasArg(options::OPT_shared)) {
      CmdArgs.push_back("-Bshareable");
    } else {
      CmdArgs.push_back("-dynamic-linker");
      CmdArgs.push_back("/sbin/ldyldo");
    }
    CmdArgs.push_back("--enable-new-dtags");
  }

  CmdArgs.push_back("-o");
  CmdArgs.push_back(Output.getFilename());

  // standard C library
  /*
  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles)) {
    if (!Args.hasArg(options::OPT_shared)) {
      CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("Scrt1.o")));
    }
  }
  */

  // c library startup files
  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles)) {
    // static executable entry point
    if (Args.hasArg(options::OPT_static)) {
      CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("crt0T.o")));
    }
    // shared library or position-independent executable entry point 
    else if (Args.hasArg(options::OPT_shared) || IsPIE) {
      CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("crt0S.o")));
    }
    // regular executable entry point
    else {
      CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("crt0.o")));
    }

    // static executables need the C initializer (_init/_fini) as well
    if(Args.hasArg(options::OPT_static)) {
      CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("crti.o")));
    }
  }
 

  Args.AddAllArgs(CmdArgs, options::OPT_L);
  Args.AddAllArgs(CmdArgs, options::OPT_u);

  ToolChain.AddFilePathLibArgs(Args, CmdArgs);

  if (D.isUsingLTO()) {
    assert(!Inputs.empty() && "Must have at least one input.");
    addLTOOptions(ToolChain, Args, CmdArgs, Output, Inputs[0],
                  D.getLTOMode() == LTOK_Thin);
  }

  AddLinkerInputs(ToolChain, Inputs, Args, CmdArgs, JA);

  // include some standard libraries
  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs)) {
    if (Args.hasArg(options::OPT_static)) {
      CmdArgs.push_back("-Bstatic");
    }

    if (D.CCCIsCXX()) {
      if (ToolChain.ShouldLinkCXXStdlib(Args)) {
        bool OnlyLibstdcxxStatic = Args.hasArg(options::OPT_static_libstdcxx) &&
                                   !Args.hasArg(options::OPT_static);
        CmdArgs.push_back("--push-state");
        CmdArgs.push_back("--as-needed");
        if (OnlyLibstdcxxStatic)
          CmdArgs.push_back("-Bstatic");
        ToolChain.AddCXXStdlibLibArgs(Args, CmdArgs);
        if (OnlyLibstdcxxStatic)
          CmdArgs.push_back("-Bdynamic");

        // we always use OpenLibM for math
        // XXX: validate this with new toolchain and libraries
        CmdArgs.push_back("-lopenlibm");
        CmdArgs.push_back("--pop-state");
      }
    }

    AddRunTimeLibs(ToolChain, D, CmdArgs, Args);

    // libc will always pull in libsystem (for syscalls)
    if (!Args.hasArg(options::OPT_nolibc)){
      CmdArgs.push_back("-lc");
      // TODO: specify the right things
      // CmdArgs.push_back("-lsystem");
    }
  }

  C.addCommand(std::make_unique<Command>(JA, *this, ResponseFileSupport::None(),
                                         Exec, CmdArgs, Inputs, Output));
}

/// Kush - Kush tool chain which can call as(1) and ld(1) directly.
/**
 * Create the Kush toolchain.
 *
 * It assumes we're specified a sysroot of some sort.
 */
Kush::Kush(const Driver &D, const llvm::Triple &Triple,
                 const ArgList &Args)
    : ToolChain(D, Triple, Args) {
  getProgramPaths().push_back(getDriver().getInstalledDir());
  if (getDriver().getInstalledDir() != D.Dir)
    getProgramPaths().push_back(D.Dir);

  // default search paths for libraries
  if (!D.SysRoot.empty()) {
    SmallString<256> P(D.SysRoot);
    llvm::sys::path::append(P, "System", "Libraries");
    getFilePaths().push_back(std::string(P.str()));
  }
  if (!D.SysRoot.empty()) {
    SmallString<256> P(D.SysRoot);
    llvm::sys::path::append(P, "Local", "Libraries");
    getFilePaths().push_back(std::string(P.str()));
  }
}

std::string Kush::ComputeEffectiveClangTriple(const ArgList &Args,
                                                 types::ID InputType) const {
  llvm::Triple Triple(ComputeLLVMTriple(Args, InputType));
  return Triple.str();
}

Tool *Kush::buildLinker() const {
  return new tools::kush::Linker(*this);
}

/**
 * Determine what compiler runtime to use.
 */
ToolChain::RuntimeLibType Kush::GetRuntimeLibType(
    const ArgList &Args) const {
  if (Arg *A = Args.getLastArg(clang::driver::options::OPT_rtlib_EQ)) {
    StringRef Value = A->getValue();
    if (Value != "compiler-rt")
      getDriver().Diag(clang::diag::err_drv_invalid_rtlib_name)
          << A->getAsString(Args);
  }

  return ToolChain::RLT_CompilerRT;
}

/**
 * Add some bonus target options.
 *
 * We put functions in their own sections for better LTO performance.
 */
void Kush::addClangTargetOptions(const ArgList &DriverArgs,
                                    ArgStringList &CC1Args,
                                    Action::OffloadKind) const {
  if (!DriverArgs.hasFlag(options::OPT_fuse_init_array,
                          options::OPT_fno_use_init_array, true)) {
    CC1Args.push_back("-fno-use-init-array");
  }

    // this puts functions into their own section for better optimization
    CC1Args.push_back("-ffunction-sections");
    CC1Args.push_back("-fdata-sections");
}

/**
 * Add compile flags to find the system include files.
 */
void Kush::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                        ArgStringList &CC1Args) const {
  const Driver &D = getDriver();

  if (DriverArgs.hasArg(options::OPT_nostdinc))
    return;

  // built in includes
  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    SmallString<256> P(D.ResourceDir);
    llvm::sys::path::append(P, "include");
    addSystemInclude(DriverArgs, CC1Args, P);
  }

  if (DriverArgs.hasArg(options::OPT_nostdlibinc))
    return;

  // default system header search paths
  if (!D.SysRoot.empty()) {
    SmallString<256> P(D.SysRoot), P2(D.SysRoot);
    llvm::sys::path::append(P, "System", "Includes");
    addSystemInclude(DriverArgs, CC1Args, P.str());

    llvm::sys::path::append(P2, "Local", "Includes");
    addSystemInclude(DriverArgs, CC1Args, P2.str());
  }
}

/**
 * Add C++ standard library compile flags.
 */
void Kush::AddCXXStdlibLibArgs(const ArgList &Args,
                                  ArgStringList &CmdArgs) const {
  switch (GetCXXStdlibType(Args)) {
  // we need to use the clang libraries
  case ToolChain::CST_Libcxx:
    CmdArgs.push_back("-lc++abi");
    CmdArgs.push_back("-lc++");

    // C++ exceptions require libunwind
    if(!Args.hasArg(options::OPT_static)) {
      CmdArgs.push_back("-lunwind");
    }
    break;

  // other C++ libraries aren't supported
  default:
    llvm_unreachable("invalid stdlib name");
  }
}

/**
 * Add include paths for C++ library.
 */
void Kush::AddClangCXXStdlibIncludeArgs(const ArgList &DriverArgs,
                                           ArgStringList &CC1Args) const {
  const Driver &D = getDriver();

  // bail if no standard library includes
  if (DriverArgs.hasArg(options::OPT_nostdlibinc) ||
      DriverArgs.hasArg(options::OPT_nostdincxx))
    return;

  switch (GetCXXStdlibType(DriverArgs)) {
  case ToolChain::CST_Libcxx: {
    SmallString<256> P(D.SysRoot);
    llvm::sys::path::append(P, "System", "Includes", "c++", "v1");
    addSystemInclude(DriverArgs, CC1Args, P.str());
    break;
  }

  default:
    llvm_unreachable("invalid stdlib name");
  }
}
