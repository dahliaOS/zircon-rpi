// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package fragments

const EventSender = `
{{- define "EventSenderDeclaration" }}
class {{ .Name }}::EventSender {
 public:
  {{- range FilterMethodsWithReqs .Methods }}
  zx_status_t {{ .Name }}({{ template "Params" .Response }}) {
    if (auto _binding = binding_.lock()) {
      return Send{{ .Name }}Event(_binding->channel() {{- if .Response }}, {{ end -}} {{ template "SyncClientMoveParams" .Response }});
    }
    return ZX_ERR_CANCELED;
  }

    {{- if .Response }}
{{ "" }}
  zx_status_t {{ .Name }}(::fidl::BytePart _buffer, {{ template "Params" .Response }}) {
    if (auto _binding = binding_.lock()) {
      return Send{{ .Name }}Event(_binding->channel(), std::move(_buffer), {{ template "SyncClientMoveParams" .Response }});
    }
    return ZX_ERR_CANCELED;
  }
    {{- end }}
{{ "" }}
  {{- end }}
 private:
  friend class ::fidl::ServerBindingRef<{{ .Name }}>;

  explicit EventSender(std::weak_ptr<::fidl::internal::AsyncBinding> binding)
      : binding_(std::move(binding)) {}

  std::weak_ptr<::fidl::internal::AsyncBinding> binding_;
};
{{- end }}
`
