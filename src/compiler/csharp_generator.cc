/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <cctype>
#include <map>
#include <sstream>
#include <vector>

#include "src/compiler/csharp_generator.h"
#include "src/compiler/config.h"
#include "src/compiler/csharp_generator_helpers.h"
#include "src/compiler/csharp_generator.h"


using google::protobuf::compiler::csharp::GetFileNamespace;
using google::protobuf::compiler::csharp::GetClassName;
using google::protobuf::compiler::csharp::GetReflectionClassName;
using grpc::protobuf::FileDescriptor;
using grpc::protobuf::Descriptor;
using grpc::protobuf::ServiceDescriptor;
using grpc::protobuf::MethodDescriptor;
using grpc::protobuf::io::Printer;
using grpc::protobuf::io::StringOutputStream;
using grpc_generator::MethodType;
using grpc_generator::GetMethodType;
using grpc_generator::METHODTYPE_NO_STREAMING;
using grpc_generator::METHODTYPE_CLIENT_STREAMING;
using grpc_generator::METHODTYPE_SERVER_STREAMING;
using grpc_generator::METHODTYPE_BIDI_STREAMING;
using grpc_generator::StringReplace;
using std::map;
using std::vector;


namespace grpc_csharp_generator {
namespace {

std::string GetServiceClassName(const ServiceDescriptor* service) {
  return service->name();
}

std::string GetClientInterfaceName(const ServiceDescriptor* service) {
  return "I" + service->name() + "Client";
}

std::string GetClientClassName(const ServiceDescriptor* service) {
  return service->name() + "Client";
}

std::string GetServerInterfaceName(const ServiceDescriptor* service) {
  return "I" + service->name();
}

std::string GetServerClassName(const ServiceDescriptor* service) {
  return service->name() + "Base";
}

std::string GetCSharpMethodType(MethodType method_type) {
  switch (method_type) {
    case METHODTYPE_NO_STREAMING:
      return "MethodType.Unary";
    case METHODTYPE_CLIENT_STREAMING:
      return "MethodType.ClientStreaming";
    case METHODTYPE_SERVER_STREAMING:
      return "MethodType.ServerStreaming";
    case METHODTYPE_BIDI_STREAMING:
      return "MethodType.DuplexStreaming";
  }
  GOOGLE_LOG(FATAL)<< "Can't get here.";
  return "";
}

std::string GetServiceNameFieldName() {
  return "__ServiceName";
}

std::string GetMarshallerFieldName(const Descriptor *message) {
  return "__Marshaller_" + message->name();
}

std::string GetMethodFieldName(const MethodDescriptor *method) {
  return "__Method_" + method->name();
}

std::string GetMethodRequestParamMaybe(const MethodDescriptor *method,
                                       bool invocation_param=false) {
  if (method->client_streaming()) {
    return "";
  }
  if (invocation_param) {
    return "request, ";
  }
  return GetClassName(method->input_type()) + " request, ";
}

std::string GetMethodReturnTypeClient(const MethodDescriptor *method) {
  switch (GetMethodType(method)) {
    case METHODTYPE_NO_STREAMING:
      return "AsyncUnaryCall<" + GetClassName(method->output_type()) + ">";
    case METHODTYPE_CLIENT_STREAMING:
      return "AsyncClientStreamingCall<" + GetClassName(method->input_type())
          + ", " + GetClassName(method->output_type()) + ">";
    case METHODTYPE_SERVER_STREAMING:
      return "AsyncServerStreamingCall<" + GetClassName(method->output_type())
          + ">";
    case METHODTYPE_BIDI_STREAMING:
      return "AsyncDuplexStreamingCall<" + GetClassName(method->input_type())
          + ", " + GetClassName(method->output_type()) + ">";
  }
  GOOGLE_LOG(FATAL)<< "Can't get here.";
  return "";
}

std::string GetMethodRequestParamServer(const MethodDescriptor *method) {
  switch (GetMethodType(method)) {
    case METHODTYPE_NO_STREAMING:
    case METHODTYPE_SERVER_STREAMING:
      return GetClassName(method->input_type()) + " request";
    case METHODTYPE_CLIENT_STREAMING:
    case METHODTYPE_BIDI_STREAMING:
      return "IAsyncStreamReader<" + GetClassName(method->input_type())
          + "> requestStream";
  }
  GOOGLE_LOG(FATAL)<< "Can't get here.";
  return "";
}

std::string GetMethodReturnTypeServer(const MethodDescriptor *method) {
  switch (GetMethodType(method)) {
    case METHODTYPE_NO_STREAMING:
    case METHODTYPE_CLIENT_STREAMING:
      return "Task<" + GetClassName(method->output_type()) + ">";
    case METHODTYPE_SERVER_STREAMING:
    case METHODTYPE_BIDI_STREAMING:
      return "Task";
  }
  GOOGLE_LOG(FATAL)<< "Can't get here.";
  return "";
}

std::string GetMethodResponseStreamMaybe(const MethodDescriptor *method) {
  switch (GetMethodType(method)) {
    case METHODTYPE_NO_STREAMING:
    case METHODTYPE_CLIENT_STREAMING:
      return "";
    case METHODTYPE_SERVER_STREAMING:
    case METHODTYPE_BIDI_STREAMING:
      return ", IServerStreamWriter<" + GetClassName(method->output_type())
          + "> responseStream";
  }
  GOOGLE_LOG(FATAL)<< "Can't get here.";
  return "";
}

// Gets vector of all messages used as input or output types.
std::vector<const Descriptor*> GetUsedMessages(
    const ServiceDescriptor *service) {
  std::set<const Descriptor*> descriptor_set;
  std::vector<const Descriptor*> result;  // vector is to maintain stable ordering
  for (int i = 0; i < service->method_count(); i++) {
    const MethodDescriptor *method = service->method(i);
    if (descriptor_set.find(method->input_type()) == descriptor_set.end()) {
      descriptor_set.insert(method->input_type());
      result.push_back(method->input_type());
    }
    if (descriptor_set.find(method->output_type()) == descriptor_set.end()) {
      descriptor_set.insert(method->output_type());
      result.push_back(method->output_type());
    }
  }
  return result;
}

void GenerateMarshallerFields(Printer* out, const ServiceDescriptor *service) {
  std::vector<const Descriptor*> used_messages = GetUsedMessages(service);
  for (size_t i = 0; i < used_messages.size(); i++) {
    const Descriptor *message = used_messages[i];
    out->Print(
        "static readonly Marshaller<$type$> $fieldname$ = Marshallers.Create((arg) => global::Google.Protobuf.MessageExtensions.ToByteArray(arg), $type$.Parser.ParseFrom);\n",
        "fieldname", GetMarshallerFieldName(message), "type",
        GetClassName(message));
  }
  out->Print("\n");
}

void GenerateStaticMethodField(Printer* out, const MethodDescriptor *method) {
  out->Print(
      "static readonly Method<$request$, $response$> $fieldname$ = new Method<$request$, $response$>(\n",
      "fieldname", GetMethodFieldName(method), "request",
      GetClassName(method->input_type()), "response",
      GetClassName(method->output_type()));
  out->Indent();
  out->Indent();
  out->Print("$methodtype$,\n", "methodtype",
             GetCSharpMethodType(GetMethodType(method)));
  out->Print("$servicenamefield$,\n", "servicenamefield",
               GetServiceNameFieldName());
  out->Print("\"$methodname$\",\n", "methodname", method->name());
  out->Print("$requestmarshaller$,\n", "requestmarshaller",
             GetMarshallerFieldName(method->input_type()));
  out->Print("$responsemarshaller$);\n", "responsemarshaller",
             GetMarshallerFieldName(method->output_type()));
  out->Print("\n");
  out->Outdent();
  out->Outdent();
}

void GenerateServiceDescriptorProperty(Printer* out, const ServiceDescriptor *service) {
  std::ostringstream index;
  index << service->index();
  out->Print("// service descriptor\n");
  out->Print("public static global::Google.Protobuf.Reflection.ServiceDescriptor Descriptor\n");
  out->Print("{\n");
  out->Print("  get { return $umbrella$.Descriptor.Services[$index$]; }\n",
             "umbrella", GetReflectionClassName(service->file()), "index",
             index.str());
  out->Print("}\n");
  out->Print("\n");
}

void GenerateClientInterface(Printer* out, const ServiceDescriptor *service) {
  out->Print("// client interface\n");
  out->Print("[System.Obsolete(\"Client side interfaced will be removed "
             "in the next release. Use client class directly.\")]\n");
  out->Print("public interface $name$\n", "name",
             GetClientInterfaceName(service));
  out->Print("{\n");
  out->Indent();
  for (int i = 0; i < service->method_count(); i++) {
    const MethodDescriptor *method = service->method(i);
    MethodType method_type = GetMethodType(method);

    if (method_type == METHODTYPE_NO_STREAMING) {
      // unary calls have an extra synchronous stub method
      out->Print(
          "$response$ $methodname$($request$ request, Metadata headers = null, DateTime? deadline = null, CancellationToken cancellationToken = default(CancellationToken));\n",
          "methodname", method->name(), "request",
          GetClassName(method->input_type()), "response",
          GetClassName(method->output_type()));

      // overload taking CallOptions as a param
      out->Print(
          "$response$ $methodname$($request$ request, CallOptions options);\n",
          "methodname", method->name(), "request",
          GetClassName(method->input_type()), "response",
          GetClassName(method->output_type()));
    }

    std::string method_name = method->name();
    if (method_type == METHODTYPE_NO_STREAMING) {
      method_name += "Async";  // prevent name clash with synchronous method.
    }
    out->Print(
        "$returntype$ $methodname$($request_maybe$Metadata headers = null, DateTime? deadline = null, CancellationToken cancellationToken = default(CancellationToken));\n",
        "methodname", method_name, "request_maybe",
        GetMethodRequestParamMaybe(method), "returntype",
        GetMethodReturnTypeClient(method));

    // overload taking CallOptions as a param
    out->Print(
        "$returntype$ $methodname$($request_maybe$CallOptions options);\n",
        "methodname", method_name, "request_maybe",
        GetMethodRequestParamMaybe(method), "returntype",
        GetMethodReturnTypeClient(method));
  }
  out->Outdent();
  out->Print("}\n");
  out->Print("\n");
}

void GenerateServerInterface(Printer* out, const ServiceDescriptor *service) {
  out->Print("// server-side interface\n");
  out->Print("[System.Obsolete(\"Service implementations should inherit"
      " from the generated abstract base class instead.\")]\n");
  out->Print("public interface $name$\n", "name",
             GetServerInterfaceName(service));
  out->Print("{\n");
  out->Indent();
  for (int i = 0; i < service->method_count(); i++) {
    const MethodDescriptor *method = service->method(i);
    out->Print(
        "$returntype$ $methodname$($request$$response_stream_maybe$, "
        "ServerCallContext context);\n",
        "methodname", method->name(), "returntype",
        GetMethodReturnTypeServer(method), "request",
        GetMethodRequestParamServer(method), "response_stream_maybe",
        GetMethodResponseStreamMaybe(method));
  }
  out->Outdent();
  out->Print("}\n");
  out->Print("\n");
}

void GenerateServerClass(Printer* out, const ServiceDescriptor *service) {
  out->Print("// server-side abstract class\n");
  out->Print("public abstract class $name$\n", "name",
             GetServerClassName(service));
  out->Print("{\n");
  out->Indent();
  for (int i = 0; i < service->method_count(); i++) {
    const MethodDescriptor *method = service->method(i);
    out->Print(
        "public virtual $returntype$ $methodname$($request$$response_stream_maybe$, "
        "ServerCallContext context)\n",
        "methodname", method->name(), "returntype",
        GetMethodReturnTypeServer(method), "request",
        GetMethodRequestParamServer(method), "response_stream_maybe",
        GetMethodResponseStreamMaybe(method));
    out->Print("{\n");
    out->Indent();
    out->Print("throw new RpcException("
               "new Status(StatusCode.Unimplemented, \"\"));\n");
    out->Outdent();
    out->Print("}\n\n");
  }
  out->Outdent();
  out->Print("}\n");
  out->Print("\n");
}

void GenerateClientStub(Printer* out, const ServiceDescriptor *service) {
  out->Print("// client stub\n");
  out->Print("#pragma warning disable 0618\n");
  out->Print(
      "public class $name$ : ClientBase<$name$>, $interface$\n",
      "name", GetClientClassName(service),
      "interface", GetClientInterfaceName(service));
  out->Print("#pragma warning restore 0618\n");
  out->Print("{\n");
  out->Indent();

  // constructors
  out->Print("public $name$(Channel channel) : base(channel)\n",
             "name", GetClientClassName(service));
  out->Print("{\n");
  out->Print("}\n");
  out->Print("public $name$(CallInvoker callInvoker) : base(callInvoker)\n",
             "name", GetClientClassName(service));
  out->Print("{\n");
  out->Print("}\n");
  out->Print("///<summary>Protected parameterless constructor to allow creation"
             " of test doubles.</summary>\n");
  out->Print("protected $name$() : base()\n",
             "name", GetClientClassName(service));
  out->Print("{\n");
  out->Print("}\n");
  out->Print("///<summary>Protected constructor to allow creation of configured"
             " clients.</summary>\n");
  out->Print("protected $name$(ClientBaseConfiguration configuration)"
             " : base(configuration)\n",
             "name", GetClientClassName(service));
  out->Print("{\n");
  out->Print("}\n\n");

  for (int i = 0; i < service->method_count(); i++) {
    const MethodDescriptor *method = service->method(i);
    MethodType method_type = GetMethodType(method);

    if (method_type == METHODTYPE_NO_STREAMING) {
      // unary calls have an extra synchronous stub method
      out->Print("public virtual $response$ $methodname$($request$ request, Metadata headers = null, DateTime? deadline = null, CancellationToken cancellationToken = default(CancellationToken))\n",
          "methodname", method->name(), "request",
          GetClassName(method->input_type()), "response",
          GetClassName(method->output_type()));
      out->Print("{\n");
      out->Indent();
      out->Print("return $methodname$(request, new CallOptions(headers, deadline, cancellationToken));\n",
                 "methodname", method->name());
      out->Outdent();
      out->Print("}\n");

      // overload taking CallOptions as a param
      out->Print("public virtual $response$ $methodname$($request$ request, CallOptions options)\n",
          "methodname", method->name(), "request",
          GetClassName(method->input_type()), "response",
          GetClassName(method->output_type()));
      out->Print("{\n");
      out->Indent();
      out->Print("return CallInvoker.BlockingUnaryCall($methodfield$, null, options, request);\n",
                 "methodfield", GetMethodFieldName(method));
      out->Outdent();
      out->Print("}\n");
    }

    std::string method_name = method->name();
    if (method_type == METHODTYPE_NO_STREAMING) {
      method_name += "Async";  // prevent name clash with synchronous method.
    }
    out->Print(
            "public virtual $returntype$ $methodname$($request_maybe$Metadata headers = null, DateTime? deadline = null, CancellationToken cancellationToken = default(CancellationToken))\n",
            "methodname", method_name, "request_maybe",
            GetMethodRequestParamMaybe(method), "returntype",
            GetMethodReturnTypeClient(method));
    out->Print("{\n");
    out->Indent();

    out->Print("return $methodname$($request_maybe$new CallOptions(headers, deadline, cancellationToken));\n",
               "methodname", method_name,
               "request_maybe", GetMethodRequestParamMaybe(method, true));
    out->Outdent();
    out->Print("}\n");

    // overload taking CallOptions as a param
    out->Print(
        "public virtual $returntype$ $methodname$($request_maybe$CallOptions options)\n",
        "methodname", method_name, "request_maybe",
        GetMethodRequestParamMaybe(method), "returntype",
        GetMethodReturnTypeClient(method));
    out->Print("{\n");
    out->Indent();
    switch (GetMethodType(method)) {
      case METHODTYPE_NO_STREAMING:
        out->Print("return CallInvoker.AsyncUnaryCall($methodfield$, null, options, request);\n",
                   "methodfield", GetMethodFieldName(method));
        break;
      case METHODTYPE_CLIENT_STREAMING:
        out->Print("return CallInvoker.AsyncClientStreamingCall($methodfield$, null, options);\n",
                   "methodfield", GetMethodFieldName(method));
        break;
      case METHODTYPE_SERVER_STREAMING:
        out->Print(
            "return CallInvoker.AsyncServerStreamingCall($methodfield$, null, options, request);\n",
            "methodfield", GetMethodFieldName(method));
        break;
      case METHODTYPE_BIDI_STREAMING:
        out->Print("return CallInvoker.AsyncDuplexStreamingCall($methodfield$, null, options);\n",
                   "methodfield", GetMethodFieldName(method));
        break;
      default:
        GOOGLE_LOG(FATAL)<< "Can't get here.";
    }
    out->Outdent();
    out->Print("}\n");
  }

  // override NewInstance method
  out->Print("protected override $name$ NewInstance(ClientBaseConfiguration configuration)\n",
             "name", GetClientClassName(service));
  out->Print("{\n");
  out->Indent();
  out->Print("return new $name$(configuration);\n",
             "name", GetClientClassName(service));
  out->Outdent();
  out->Print("}\n");

  out->Outdent();
  out->Print("}\n");
  out->Print("\n");
}

void GenerateBindServiceMethod(Printer* out, const ServiceDescriptor *service,
                               bool use_server_class) {
  out->Print(
      "// creates service definition that can be registered with a server\n");
  out->Print("#pragma warning disable 0618\n");
  out->Print(
      "public static ServerServiceDefinition BindService($interface$ serviceImpl)\n",
      "interface", use_server_class ? GetServerClassName(service) :
          GetServerInterfaceName(service));
  out->Print("#pragma warning restore 0618\n");
  out->Print("{\n");
  out->Indent();

  out->Print(
      "return ServerServiceDefinition.CreateBuilder($servicenamefield$)\n",
      "servicenamefield", GetServiceNameFieldName());
  out->Indent();
  out->Indent();
  for (int i = 0; i < service->method_count(); i++) {
    const MethodDescriptor *method = service->method(i);
    out->Print(".AddMethod($methodfield$, serviceImpl.$methodname$)",
               "methodfield", GetMethodFieldName(method), "methodname",
               method->name());
    if (i == service->method_count() - 1) {
      out->Print(".Build();");
    }
    out->Print("\n");
  }
  out->Outdent();
  out->Outdent();

  out->Outdent();
  out->Print("}\n");
  out->Print("\n");
}

void GenerateNewStubMethods(Printer* out, const ServiceDescriptor *service) {
  out->Print("// creates a new client\n");
  out->Print("public static $classname$ NewClient(Channel channel)\n",
             "classname", GetClientClassName(service));
  out->Print("{\n");
  out->Indent();
  out->Print("return new $classname$(channel);\n", "classname",
             GetClientClassName(service));
  out->Outdent();
  out->Print("}\n");
  out->Print("\n");
}

void GenerateService(Printer* out, const ServiceDescriptor *service) {
  out->Print("public static class $classname$\n", "classname",
             GetServiceClassName(service));
  out->Print("{\n");
  out->Indent();
  out->Print("static readonly string $servicenamefield$ = \"$servicename$\";\n",
             "servicenamefield", GetServiceNameFieldName(), "servicename",
             service->full_name());
  out->Print("\n");

  GenerateMarshallerFields(out, service);
  for (int i = 0; i < service->method_count(); i++) {
    GenerateStaticMethodField(out, service->method(i));
  }
  GenerateServiceDescriptorProperty(out, service);
  GenerateClientInterface(out, service);
  GenerateServerInterface(out, service);
  GenerateServerClass(out, service);
  GenerateClientStub(out, service);
  GenerateBindServiceMethod(out, service, false);
  GenerateBindServiceMethod(out, service, true);
  GenerateNewStubMethods(out, service);

  out->Outdent();
  out->Print("}\n");
}

}  // anonymous namespace

grpc::string GetServices(const FileDescriptor *file) {
  grpc::string output;
  {
    // Scope the output stream so it closes and finalizes output to the string.

    StringOutputStream output_stream(&output);
    Printer out(&output_stream, '$');

    // Don't write out any output if there no services, to avoid empty service
    // files being generated for proto files that don't declare any.
    if (file->service_count() == 0) {
      return output;
    }

    // Write out a file header.
    out.Print("// Generated by the protocol buffer compiler.  DO NOT EDIT!\n");
    out.Print("// source: $filename$\n", "filename", file->name());
    out.Print("#region Designer generated code\n");
    out.Print("\n");
    out.Print("using System;\n");
    out.Print("using System.Threading;\n");
    out.Print("using System.Threading.Tasks;\n");
    out.Print("using Grpc.Core;\n");
    out.Print("\n");

    out.Print("namespace $namespace$ {\n", "namespace", GetFileNamespace(file));
    out.Indent();
    for (int i = 0; i < file->service_count(); i++) {
      GenerateService(&out, file->service(i));
    }
    out.Outdent();
    out.Print("}\n");
    out.Print("#endregion\n");
  }
  return output;
}

}  // namespace grpc_csharp_generator
