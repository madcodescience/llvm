//===- PassManager.h - Pass management infrastructure -----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This header defines various interfaces for pass management in LLVM. There
/// is no "pass" interface in LLVM per se. Instead, an instance of any class
/// which supports a method to 'run' it over a unit of IR can be used as
/// a pass. A pass manager is generally a tool to collect a sequence of passes
/// which run over a particular IR construct, and run each of them in sequence
/// over each such construct in the containing IR construct. As there is no
/// containing IR construct for a Module, a manager for passes over modules
/// forms the base case which runs its managed passes in sequence over the
/// single module provided.
///
/// The core IR library provides managers for running passes over
/// modules and functions.
///
/// * FunctionPassManager can run over a Module, runs each pass over
///   a Function.
/// * ModulePassManager must be directly run, runs each pass over the Module.
///
/// Note that the implementations of the pass managers use concept-based
/// polymorphism as outlined in the "Value Semantics and Concept-based
/// Polymorphism" talk (or its abbreviated sibling "Inheritance Is The Base
/// Class of Evil") by Sean Parent:
/// * http://github.com/sean-parent/sean-parent.github.com/wiki/Papers-and-Presentations
/// * http://www.youtube.com/watch?v=_BpMYeUFXv8
/// * http://channel9.msdn.com/Events/GoingNative/2013/Inheritance-Is-The-Base-Class-of-Evil
///
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/polymorphic_ptr.h"
#include "llvm/Support/type_traits.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include <list>
#include <vector>

namespace llvm {

class Module;
class Function;

/// \brief An abstract set of preserved analyses following a transformation pass
/// run.
///
/// When a transformation pass is run, it can return a set of analyses whose
/// results were preserved by that transformation. The default set is "none",
/// and preserving analyses must be done explicitly.
///
/// There is also an explicit all state which can be used (for example) when
/// the IR is not mutated at all.
class PreservedAnalyses {
public:
  /// \brief Convenience factory function for the empty preserved set.
  static PreservedAnalyses none() { return PreservedAnalyses(); }

  /// \brief Construct a special preserved set that preserves all passes.
  static PreservedAnalyses all() {
    PreservedAnalyses PA;
    PA.PreservedPassIDs.insert((void *)AllPassesID);
    return PA;
  }

  PreservedAnalyses &operator=(PreservedAnalyses Arg) {
    swap(Arg);
    return *this;
  }

  void swap(PreservedAnalyses &Arg) {
    PreservedPassIDs.swap(Arg.PreservedPassIDs);
  }

  /// \brief Mark a particular pass as preserved, adding it to the set.
  template <typename PassT> void preserve() {
    if (!areAllPreserved())
      PreservedPassIDs.insert(PassT::ID());
  }

  /// \brief Intersect this set with another in place.
  ///
  /// This is a mutating operation on this preserved set, removing all
  /// preserved passes which are not also preserved in the argument.
  void intersect(const PreservedAnalyses &Arg) {
    if (Arg.areAllPreserved())
      return;
    if (areAllPreserved()) {
      PreservedPassIDs = Arg.PreservedPassIDs;
      return;
    }
    for (SmallPtrSet<void *, 2>::const_iterator I = PreservedPassIDs.begin(),
                                                E = PreservedPassIDs.end();
         I != E; ++I)
      if (!Arg.PreservedPassIDs.count(*I))
        PreservedPassIDs.erase(*I);
  }

#if LLVM_HAS_RVALUE_REFERENCES
  /// \brief Intersect this set with a temporary other set in place.
  ///
  /// This is a mutating operation on this preserved set, removing all
  /// preserved passes which are not also preserved in the argument.
  void intersect(PreservedAnalyses &&Arg) {
    if (Arg.areAllPreserved())
      return;
    if (areAllPreserved()) {
      PreservedPassIDs = std::move(Arg.PreservedPassIDs);
      return;
    }
    for (SmallPtrSet<void *, 2>::const_iterator I = PreservedPassIDs.begin(),
                                                E = PreservedPassIDs.end();
         I != E; ++I)
      if (!Arg.PreservedPassIDs.count(*I))
        PreservedPassIDs.erase(*I);
  }
#endif

  /// \brief Query whether a pass is marked as preserved by this set.
  template <typename PassT> bool preserved() const {
    return preserved(PassT::ID());
  }

  /// \brief Query whether an abstract pass ID is marked as preserved by this
  /// set.
  bool preserved(void *PassID) const {
    return PreservedPassIDs.count((void *)AllPassesID) ||
           PreservedPassIDs.count(PassID);
  }

private:
  // Note that this must not be -1 or -2 as those are already used by the
  // SmallPtrSet.
  static const uintptr_t AllPassesID = (intptr_t)-3;

  bool areAllPreserved() const { return PreservedPassIDs.count((void *)AllPassesID); }

  SmallPtrSet<void *, 2> PreservedPassIDs;
};

inline void swap(PreservedAnalyses &LHS, PreservedAnalyses &RHS) {
  LHS.swap(RHS);
}

/// \brief Implementation details of the pass manager interfaces.
namespace detail {

/// \brief Template for the abstract base class used to dispatch
/// polymorphically over pass objects.
template <typename T> struct PassConcept {
  // Boiler plate necessary for the container of derived classes.
  virtual ~PassConcept() {}
  virtual PassConcept *clone() = 0;

  /// \brief The polymorphic API which runs the pass over a given IR entity.
  virtual PreservedAnalyses run(T Arg) = 0;
};

/// \brief A template wrapper used to implement the polymorphic API.
///
/// Can be instantiated for any object which provides a \c run method
/// accepting a \c T. It requires the pass to be a copyable
/// object.
template <typename T, typename PassT> struct PassModel : PassConcept<T> {
  PassModel(PassT Pass) : Pass(llvm_move(Pass)) {}
  virtual PassModel *clone() { return new PassModel(Pass); }
  virtual PreservedAnalyses run(T Arg) { return Pass.run(Arg); }
  PassT Pass;
};

/// \brief Abstract concept of an analysis result.
///
/// This concept is parameterized over the IR unit that this result pertains
/// to.
template <typename IRUnitT> struct AnalysisResultConcept {
  virtual ~AnalysisResultConcept() {}
  virtual AnalysisResultConcept *clone() = 0;

  /// \brief Method to try and mark a result as invalid.
  ///
  /// When the outer analysis manager detects a change in some underlying
  /// unit of the IR, it will call this method on all of the results cached.
  ///
  /// This method also receives a set of preserved analyses which can be used
  /// to avoid invalidation because the pass which changed the underlying IR
  /// took care to update or preserve the analysis result in some way.
  ///
  /// \returns true if the result is indeed invalid (the default).
  virtual bool invalidate(IRUnitT *IR, const PreservedAnalyses &PA) = 0;
};

/// \brief Wrapper to model the analysis result concept.
///
/// By default, this will implement the invalidate method with a trivial
/// implementation so that the actual analysis result doesn't need to provide
/// an invalidation handler. It is only selected when the invalidation handler
/// is not part of the ResultT's interface.
template <typename IRUnitT, typename PassT, typename ResultT,
          bool HasInvalidateHandler = false>
struct AnalysisResultModel : AnalysisResultConcept<IRUnitT> {
  AnalysisResultModel(ResultT Result) : Result(llvm_move(Result)) {}
  virtual AnalysisResultModel *clone() {
    return new AnalysisResultModel(Result);
  }

  /// \brief The model bases invalidation solely on being in the preserved set.
  //
  // FIXME: We should actually use two different concepts for analysis results
  // rather than two different models, and avoid the indirect function call for
  // ones that use the trivial behavior.
  virtual bool invalidate(IRUnitT *, const PreservedAnalyses &PA) {
    return !PA.preserved(PassT::ID());
  }

  ResultT Result;
};

/// \brief Wrapper to model the analysis result concept.
///
/// Can wrap any type which implements a suitable invalidate member and model
/// the AnalysisResultConcept for the AnalysisManager.
template <typename IRUnitT, typename PassT, typename ResultT>
struct AnalysisResultModel<IRUnitT, PassT, ResultT,
                           true> : AnalysisResultConcept<IRUnitT> {
  AnalysisResultModel(ResultT Result) : Result(llvm_move(Result)) {}
  virtual AnalysisResultModel *clone() {
    return new AnalysisResultModel(Result);
  }

  /// \brief The model delegates to the \c ResultT method.
  virtual bool invalidate(IRUnitT *IR, const PreservedAnalyses &PA) {
    return Result.invalidate(IR, PA);
  }

  ResultT Result;
};

/// \brief SFINAE metafunction for computing whether \c ResultT provides an
/// \c invalidate member function.
template <typename IRUnitT, typename ResultT> class ResultHasInvalidateMethod {
  typedef char SmallType;
  struct BigType { char a, b; };

  template <typename T, bool (T::*)(IRUnitT *, const PreservedAnalyses &)>
  struct Checker;

  template <typename T> static SmallType f(Checker<T, &T::invalidate> *);
  template <typename T> static BigType f(...);

public:
  enum { Value = sizeof(f<ResultT>(0)) == sizeof(SmallType) };
};

/// \brief Abstract concept of an analysis pass.
///
/// This concept is parameterized over the IR unit that it can run over and
/// produce an analysis result.
template <typename IRUnitT> struct AnalysisPassConcept {
  virtual ~AnalysisPassConcept() {}
  virtual AnalysisPassConcept *clone() = 0;

  /// \brief Method to run this analysis over a unit of IR.
  /// \returns The analysis result object to be queried by users, the caller
  /// takes ownership.
  virtual AnalysisResultConcept<IRUnitT> *run(IRUnitT *IR) = 0;
};

/// \brief Wrapper to model the analysis pass concept.
///
/// Can wrap any type which implements a suitable \c run method. The method
/// must accept the IRUnitT as an argument and produce an object which can be
/// wrapped in a \c AnalysisResultModel.
template <typename PassT>
struct AnalysisPassModel : AnalysisPassConcept<typename PassT::IRUnitT> {
  AnalysisPassModel(PassT Pass) : Pass(llvm_move(Pass)) {}
  virtual AnalysisPassModel *clone() { return new AnalysisPassModel(Pass); }

  // FIXME: Replace PassT::IRUnitT with type traits when we use C++11.
  typedef typename PassT::IRUnitT IRUnitT;

  // FIXME: Replace PassT::Result with type traits when we use C++11.
  typedef AnalysisResultModel<
      IRUnitT, PassT, typename PassT::Result,
      ResultHasInvalidateMethod<IRUnitT, typename PassT::Result>::Value>
          ResultModelT;

  /// \brief The model delegates to the \c PassT::run method.
  ///
  /// The return is wrapped in an \c AnalysisResultModel.
  virtual ResultModelT *run(IRUnitT *IR) {
    return new ResultModelT(Pass.run(IR));
  }

  PassT Pass;
};

}

class ModuleAnalysisManager;

class ModulePassManager {
public:
  explicit ModulePassManager(ModuleAnalysisManager *AM = 0) : AM(AM) {}

  /// \brief Run all of the module passes in this module pass manager over
  /// a module.
  ///
  /// This method should only be called for a single module as there is the
  /// expectation that the lifetime of a pass is bounded to that of a module.
  PreservedAnalyses run(Module *M);

  template <typename ModulePassT> void addPass(ModulePassT Pass) {
    Passes.push_back(new ModulePassModel<ModulePassT>(llvm_move(Pass)));
  }

private:
  // Pull in the concept type and model template specialized for modules.
  typedef detail::PassConcept<Module *> ModulePassConcept;
  template <typename PassT>
  struct ModulePassModel : detail::PassModel<Module *, PassT> {
    ModulePassModel(PassT Pass) : detail::PassModel<Module *, PassT>(Pass) {}
  };

  ModuleAnalysisManager *AM;
  std::vector<polymorphic_ptr<ModulePassConcept> > Passes;
};

class FunctionAnalysisManager;

class FunctionPassManager {
public:
  explicit FunctionPassManager(FunctionAnalysisManager *AM = 0) : AM(AM) {}

  template <typename FunctionPassT> void addPass(FunctionPassT Pass) {
    Passes.push_back(new FunctionPassModel<FunctionPassT>(llvm_move(Pass)));
  }

  PreservedAnalyses run(Function *F);

private:
  // Pull in the concept type and model template specialized for functions.
  typedef detail::PassConcept<Function *> FunctionPassConcept;
  template <typename PassT>
  struct FunctionPassModel : detail::PassModel<Function *, PassT> {
    FunctionPassModel(PassT Pass)
        : detail::PassModel<Function *, PassT>(Pass) {}
  };

  FunctionAnalysisManager *AM;
  std::vector<polymorphic_ptr<FunctionPassConcept> > Passes;
};

/// \brief A module analysis pass manager with lazy running and caching of
/// results.
class ModuleAnalysisManager {
public:
  ModuleAnalysisManager() {}

  /// \brief Get the result of an analysis pass for this module.
  ///
  /// If there is not a valid cached result in the manager already, this will
  /// re-run the analysis to produce a valid result.
  template <typename PassT> const typename PassT::Result &getResult(Module *M) {
    LLVM_STATIC_ASSERT((is_same<typename PassT::IRUnitT, Module>::value),
                       "The analysis pass must be over a Module.");
    assert(ModuleAnalysisPasses.count(PassT::ID()) &&
           "This analysis pass was not registered prior to being queried");

    const detail::AnalysisResultConcept<Module> &ResultConcept =
        getResultImpl(PassT::ID(), M);
    typedef detail::AnalysisResultModel<
        Module, PassT, typename PassT::Result,
        detail::ResultHasInvalidateMethod<
            Module, typename PassT::Result>::Value> ResultModelT;
    return static_cast<const ResultModelT &>(ResultConcept).Result;
  }

  /// \brief Register an analysis pass with the manager.
  ///
  /// This provides an initialized and set-up analysis pass to the
  /// analysis
  /// manager. Whomever is setting up analysis passes must use this to
  /// populate
  /// the manager with all of the analysis passes available.
  template <typename PassT> void registerPass(PassT Pass) {
    LLVM_STATIC_ASSERT((is_same<typename PassT::IRUnitT, Module>::value),
                       "The analysis pass must be over a Module.");
    assert(!ModuleAnalysisPasses.count(PassT::ID()) &&
           "Registered the same analysis pass twice!");
    ModuleAnalysisPasses[PassT::ID()] =
        new detail::AnalysisPassModel<PassT>(llvm_move(Pass));
  }

  /// \brief Invalidate a specific analysis pass for an IR module.
  ///
  /// Note that the analysis result can disregard invalidation.
  template <typename PassT> void invalidate(Module *M) {
    LLVM_STATIC_ASSERT((is_same<typename PassT::IRUnitT, Module>::value),
                       "The analysis pass must be over a Module.");
    assert(ModuleAnalysisPasses.count(PassT::ID()) &&
           "This analysis pass was not registered prior to being invalidated");
    invalidateImpl(PassT::ID(), M);
  }

  /// \brief Invalidate analyses cached for an IR Module.
  ///
  /// Walk through all of the analyses pertaining to this module and invalidate
  /// them unless they are preserved by the PreservedAnalyses set.
  void invalidate(Module *M, const PreservedAnalyses &PA);

private:
  /// \brief Get a module pass result, running the pass if necessary.
  const detail::AnalysisResultConcept<Module> &getResultImpl(void *PassID,
                                                             Module *M);

  /// \brief Invalidate a module pass result.
  void invalidateImpl(void *PassID, Module *M);

  /// \brief Map type from module analysis pass ID to pass concept pointer.
  typedef DenseMap<void *,
                   polymorphic_ptr<detail::AnalysisPassConcept<Module> > >
      ModuleAnalysisPassMapT;

  /// \brief Collection of module analysis passes, indexed by ID.
  ModuleAnalysisPassMapT ModuleAnalysisPasses;

  /// \brief Map type from module analysis pass ID to pass result concept pointer.
  typedef DenseMap<void *,
                   polymorphic_ptr<detail::AnalysisResultConcept<Module> > >
      ModuleAnalysisResultMapT;

  /// \brief Cache of computed module analysis results for this module.
  ModuleAnalysisResultMapT ModuleAnalysisResults;
};

/// \brief A function analysis manager to coordinate and cache analyses run over
/// a module.
class FunctionAnalysisManager {
public:
  FunctionAnalysisManager() {}

  /// \brief Get the result of an analysis pass for a function.
  ///
  /// If there is not a valid cached result in the manager already, this will
  /// re-run the analysis to produce a valid result.
  template <typename PassT>
  const typename PassT::Result &getResult(Function *F) {
    LLVM_STATIC_ASSERT((is_same<typename PassT::IRUnitT, Function>::value),
                       "The analysis pass must be over a Function.");
    assert(FunctionAnalysisPasses.count(PassT::ID()) &&
           "This analysis pass was not registered prior to being queried");

    const detail::AnalysisResultConcept<Function> &ResultConcept =
        getResultImpl(PassT::ID(), F);
    typedef detail::AnalysisResultModel<
        Function, PassT, typename PassT::Result,
        detail::ResultHasInvalidateMethod<
            Function, typename PassT::Result>::Value> ResultModelT;
    return static_cast<const ResultModelT &>(ResultConcept).Result;
  }

  /// \brief Register an analysis pass with the manager.
  ///
  /// This provides an initialized and set-up analysis pass to the
  /// analysis
  /// manager. Whomever is setting up analysis passes must use this to
  /// populate
  /// the manager with all of the analysis passes available.
  template <typename PassT> void registerPass(PassT Pass) {
    LLVM_STATIC_ASSERT((is_same<typename PassT::IRUnitT, Function>::value),
                       "The analysis pass must be over a Function.");
    assert(!FunctionAnalysisPasses.count(PassT::ID()) &&
           "Registered the same analysis pass twice!");
    FunctionAnalysisPasses[PassT::ID()] =
        new detail::AnalysisPassModel<PassT>(llvm_move(Pass));
  }

  /// \brief Invalidate a specific analysis pass for an IR module.
  ///
  /// Note that the analysis result can disregard invalidation.
  template <typename PassT> void invalidate(Function *F) {
    LLVM_STATIC_ASSERT((is_same<typename PassT::IRUnitT, Function>::value),
                       "The analysis pass must be over a Function.");
    assert(FunctionAnalysisPasses.count(PassT::ID()) &&
           "This analysis pass was not registered prior to being invalidated");
    invalidateImpl(PassT::ID(), F);
  }

  /// \brief Invalidate analyses cached for an IR Function.
  ///
  /// Walk through all of the analyses cache for this IR function and
  /// invalidate them unless they are preserved by the provided
  /// PreservedAnalyses set.
  void invalidate(Function *F, const PreservedAnalyses &PA);

  /// \brief Returns true if the analysis manager has an empty results cache.
  bool empty() const;

  /// \brief Clear the function analysis result cache.
  ///
  /// This routine allows cleaning up when the set of functions itself has
  /// potentially changed, and thus we can't even look up a a result and
  /// invalidate it directly. Notably, this does *not* call invalidate
  /// functions as there is nothing to be done for them.
  void clear();

private:
  /// \brief Get a function pass result, running the pass if necessary.
  const detail::AnalysisResultConcept<Function> &getResultImpl(void *PassID,
                                                               Function *F);

  /// \brief Invalidate a function pass result.
  void invalidateImpl(void *PassID, Function *F);

  /// \brief Map type from function analysis pass ID to pass concept pointer.
  typedef DenseMap<void *,
                   polymorphic_ptr<detail::AnalysisPassConcept<Function> > >
      FunctionAnalysisPassMapT;

  /// \brief Collection of function analysis passes, indexed by ID.
  FunctionAnalysisPassMapT FunctionAnalysisPasses;

  /// \brief List of function analysis pass IDs and associated concept pointers.
  ///
  /// Requires iterators to be valid across appending new entries and arbitrary
  /// erases. Provides both the pass ID and concept pointer such that it is
  /// half of a bijection and provides storage for the actual result concept.
  typedef std::list<std::pair<
      void *, polymorphic_ptr<detail::AnalysisResultConcept<Function> > > >
      FunctionAnalysisResultListT;

  /// \brief Map type from function pointer to our custom list type.
  typedef DenseMap<Function *, FunctionAnalysisResultListT>
  FunctionAnalysisResultListMapT;

  /// \brief Map from function to a list of function analysis results.
  ///
  /// Provides linear time removal of all analysis results for a function and
  /// the ultimate storage for a particular cached analysis result.
  FunctionAnalysisResultListMapT FunctionAnalysisResultLists;

  /// \brief Map type from a pair of analysis ID and function pointer to an
  /// iterator into a particular result list.
  typedef DenseMap<std::pair<void *, Function *>,
                   FunctionAnalysisResultListT::iterator>
      FunctionAnalysisResultMapT;

  /// \brief Map from an analysis ID and function to a particular cached
  /// analysis result.
  FunctionAnalysisResultMapT FunctionAnalysisResults;
};

/// \brief A module analysis which acts as a proxy for a function analysis
/// manager.
///
/// This primarily proxies invalidation information from the module analysis
/// manager and module pass manager to a function analysis manager. You should
/// never use a function analysis manager from within (transitively) a module
/// pass manager unless your parent module pass has received a proxy result
/// object for it.
///
/// FIXME: It might be really nice to "enforce" this (softly) by making this
/// proxy the API path to access a function analysis manager within a module
/// pass.
class FunctionAnalysisModuleProxy {
public:
  typedef Module IRUnitT;
  class Result;

  static void *ID() { return (void *)&PassID; }

  FunctionAnalysisModuleProxy(FunctionAnalysisManager &FAM) : FAM(FAM) {}

  /// \brief Run the analysis pass and create our proxy result object.
  ///
  /// This doesn't do any interesting work, it is primarily used to insert our
  /// proxy result object into the module analysis cache so that we can proxy
  /// invalidation to the function analysis manager.
  ///
  /// In debug builds, it will also assert that the analysis manager is empty
  /// as no queries should arrive at the function analysis manager prior to
  /// this analysis being requested.
  Result run(Module *M);

private:
  static char PassID;

  FunctionAnalysisManager &FAM;
};

/// \brief The result proxy object for the \c FunctionAnalysisModuleProxy.
///
/// See its documentation for more information.
class FunctionAnalysisModuleProxy::Result {
public:
  Result(FunctionAnalysisManager &FAM) : FAM(FAM) {}
  ~Result();

  /// \brief Handler for invalidation of the module.
  ///
  /// If this analysis itself is preserved, then we assume that the set of \c
  /// Function objects in the \c Module hasn't changed and thus we don't need
  /// to invalidate *all* cached data associated with a \c Function* in the \c
  /// FunctionAnalysisManager.
  ///
  /// Regardless of whether this analysis is marked as preserved, all of the
  /// analyses in the \c FunctionAnalysisManager are potentially invalidated
  /// based on the set of preserved analyses.
  bool invalidate(Module *M, const PreservedAnalyses &PA);

private:
  FunctionAnalysisManager &FAM;
};

/// \brief Trivial adaptor that maps from a module to its functions.
///
/// Designed to allow composition of a FunctionPass(Manager) and a
/// ModulePassManager. Note that if this pass is constructed with a pointer to
/// a \c ModuleAnalysisManager it will run the \c FunctionAnalysisModuleProxy
/// analysis prior to running the function pass over the module to enable a \c
/// FunctionAnalysisManager to be used within this run safely.
template <typename FunctionPassT>
class ModuleToFunctionPassAdaptor {
public:
  explicit ModuleToFunctionPassAdaptor(FunctionPassT Pass,
                                       ModuleAnalysisManager *MAM = 0)
      : Pass(llvm_move(Pass)), MAM(MAM) {}

  /// \brief Runs the function pass across every function in the module.
  PreservedAnalyses run(Module *M) {
    if (MAM)
      // Pull in the analysis proxy so that the function analysis manager is
      // appropriately set up.
      (void)MAM->getResult<FunctionAnalysisModuleProxy>(M);

    PreservedAnalyses PA = PreservedAnalyses::all();
    for (Module::iterator I = M->begin(), E = M->end(); I != E; ++I) {
      PreservedAnalyses PassPA = Pass.run(I);
      PA.intersect(llvm_move(PassPA));
    }

    // By definition we preserve the proxy.
    PA.preserve<FunctionAnalysisModuleProxy>();
    return PA;
  }

private:
  FunctionPassT Pass;
  ModuleAnalysisManager *MAM;
};

/// \brief A function to deduce a function pass type and wrap it in the
/// templated adaptor.
///
/// \param MAM is an optional \c ModuleAnalysisManager which (if provided) will
/// be queried for a \c FunctionAnalysisModuleProxy to enable the function
/// pass(es) to safely interact with a \c FunctionAnalysisManager.
template <typename FunctionPassT>
ModuleToFunctionPassAdaptor<FunctionPassT>
createModuleToFunctionPassAdaptor(FunctionPassT Pass,
                                  ModuleAnalysisManager *MAM = 0) {
  return ModuleToFunctionPassAdaptor<FunctionPassT>(llvm_move(Pass), MAM);
}

}
