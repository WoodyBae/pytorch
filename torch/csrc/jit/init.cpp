#include "torch/csrc/utils/pybind.h"

#include "torch/csrc/jit/python_tracer.h"
#include "torch/csrc/jit/tracer.h"
#include "torch/csrc/jit/python_ir.h"
#include "torch/csrc/jit/python_arg_flatten.h"
#include "torch/csrc/jit/export.h"
#include "torch/csrc/jit/python_compiled_function.h"
#include "torch/csrc/jit/passes/graph_fuser.h"
#include "torch/csrc/jit/passes/onnx.h"
#include "torch/csrc/jit/passes/dead_code_elimination.h"
#include "torch/csrc/jit/passes/common_subexpression_elimination.h"
#include "torch/csrc/jit/passes/peephole.h"
#include "torch/csrc/jit/passes/canonicalize.h"
#include "torch/csrc/jit/passes/onnx/peephole.h"
#include "torch/csrc/jit/graph_executor.h"
#include "torch/csrc/jit/script/init.h"
#include "torch/csrc/jit/script/python_tree_views.h"


namespace torch  { namespace jit {

namespace {

bool loadPythonClasses() {
  // Leaving this code here, because it will likely be useful at some point
  //PyObject *jit_module = PyImport_ImportModule("torch.jit");
  //THPUtils_assert(jit_module, "class loader couldn't access "
          //"torch.jit module");
  //PyObject *jit_dict = PyModule_GetDict(jit_module);

  return true;
}

template<void (*F)(std::shared_ptr<Graph>& graph)>
void graph_pass(const std::shared_ptr<tracer::TracingState>& state) {
  return F(state->graph);
}

struct PythonGraphExecutor : GraphExecutor {
  using GraphExecutor::GraphExecutor;
  variable_tensor_list run(variable_tensor_list && inputs) {
    inputs.insert(inputs.end(), captures.begin(), captures.end());
    return GraphExecutor::run(std::move(inputs));
  }
  variable_tensor_list captures;
};

// This is a temporary constructor so that we can write python tests of
// the executor. It does not have most of the functionality of CompiledFunction
// such as being able to hold parameters...
PythonGraphExecutor createExecutorByTracing(
        py::function func,
        std::vector<tracer::TraceInput> inputs,
        std::vector<tracer::TraceInput> captures,
        bool optimize) {
  std::vector<tracer::TraceInput> trace_inputs;
  trace_inputs.insert(trace_inputs.end(), inputs.begin(), inputs.end());
  trace_inputs.insert(trace_inputs.end(), captures.begin(), captures.end());
  auto enter_info = tracer::enter(std::move(trace_inputs), 1);
  py::tuple py_inputs(inputs.size());
  for(size_t i = 0; i < inputs.size(); ++i) {
    py_inputs[i] = py::cast(enter_info.second[i]);
  }
  // All conditions that could trigger this should be asserted on the Python side
  for (size_t i = 0; i < captures.size(); ++i) {
    // TODO: remove TraceInput, since everything is a Variable now
    JIT_ASSERT(captures[i].variable.defined());
    JIT_ASSERT(enter_info.second[i + inputs.size()].is_same(captures[i].variable));
  }
  // Call back into Python function
  auto out = py::reinterpret_steal<py::object>(PyObject_CallObject(func.ptr(), py_inputs.ptr()));
  if (!out)
    throw py::error_already_set();
  std::vector<autograd::Variable> outputs;
  if(PyTuple_Check(out.ptr())) {
    outputs = py::cast<std::vector<autograd::Variable>>(out);
  } else {
    outputs.push_back(py::cast<autograd::Variable>(out));
  }
  tracer::exit(outputs);
  auto graph = enter_info.first->graph;
  EliminateDeadCode(graph);
  return PythonGraphExecutor(std::move(graph), optimize);
}

// we cannot use the default py:cast<autograd::Variable> because it currently
// unwraps the data tensor in the conversion process
// TODO: replace with bs type
variable_tensor_list createVariableTensorList(py::tuple tuple, size_t reserve_extra_space = 0) {
  variable_tensor_list result;
  result.reserve(tuple.size() + reserve_extra_space);
  for(auto e : tuple) {
    result.push_back(py::cast<autograd::Variable>(e));
  }
  return result;
}

} // anonymous namespace

extern std::string runJITCPPTests();

void initJITBindings(PyObject *module) {
  auto m = py::handle(module).cast<py::module>();

  py::class_<python::IODescriptor>(m, "IODescriptor");

  m.def("_jit_init", loadPythonClasses)
   .def("_jit_pass_onnx", ToONNX)
   .def("_jit_pass_onnx_peephole", graph_pass<PeepholeOptimizeONNX>)
   .def("_jit_pass_fuse", graph_pass<FuseGraph>)
   .def("_jit_pass_dce", graph_pass<EliminateDeadCode>)
   .def("_jit_pass_cse", graph_pass<EliminateCommonSubexpression>)
   .def("_jit_pass_peephole", graph_pass<PeepholeOptimize>)
   .def("_jit_pass_canonicalize", graph_pass<Canonicalize>)
   .def("_jit_pass_lint", graph_pass<LintGraph>)
   .def("_jit_run_cpp_tests", runJITCPPTests)
   .def("_jit_flatten", [](py::handle& obj) {
     auto res =  python::flatten(obj);
     return std::make_pair(res.vars, res.desc);
   })
   .def("_jit_unflatten", [](autograd::variable_list vars, python::IODescriptor& desc) {
     return py::reinterpret_steal<py::object>(python::unflatten(vars, desc));
   });

  py::class_<PythonGraphExecutor>(m, "GraphExecutor")
      .def(
          py::init([](py::function func,
                      std::vector<tracer::TraceInput> inputs,
                      std::vector<tracer::TraceInput> captures,
                      bool optimize) {
            return createExecutorByTracing(func, std::move(inputs), std::move(captures), optimize);
          }),
          py::arg("func"),
          py::arg("inputs"),
          py::arg("captures") = std::vector<tracer::TraceInput>{},
          py::arg("optimize") = true)
      .def(
          py::init([](std::shared_ptr<Graph> graph, bool optimize) {
            return PythonGraphExecutor(std::move(graph), optimize);
          }),
          py::arg("graph"),
          py::arg("optimize") = true)
      .def("set_captures", [](PythonGraphExecutor& ge, py::args args) {
        ge.captures = createVariableTensorList(args);
      })
      .def("__call__", [](PythonGraphExecutor& ge, py::args args) -> py::object {
        auto inputs = createVariableTensorList(args, ge.captures.size());
        auto outputs = ge.run(std::move(inputs));
        // if we don't tell pybind these are variables it chokes on the
        // conversion.
        // TODO: fix conversions to be sane and make sure this works.
        if(outputs.size() == 1) {
          return py::cast(static_cast<autograd::Variable&>(outputs[0]));
        } else {
          py::tuple tuple(outputs.size());
          for(size_t i = 0; i < outputs.size(); i++) {
            tuple[i] = py::cast(static_cast<autograd::Variable&>(outputs[i]));
          }
          return tuple;
        }
      });
  initPythonIRBindings(module);
  initPythonTracerBindings(module);
  python::initCompilerMixin(module);
  script::initTreeViewBindings(module);
  script::initJitScriptBindings(module);
}

}}
