// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "node.h"
#include "handle_wrap.h"
#include "node_buffer.h"
#include "node_wrap.h"

#include <string.h>
#include <stdlib.h>

namespace node {

using v8::Array;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Handle;
using v8::HandleScope;
using v8::Integer;
using v8::Local;
using v8::Number;
using v8::Object;
using v8::String;
using v8::TryCatch;
using v8::Value;

static Cached<String> onexit_sym;


class SyncStdioPipeHandler {
 public:
  static int New(uv_loop_t* loop, int child_fd, bool readable, bool writable, uv_buf_t input_buffer, SyncStdioPipeHandler** instance_ptr) {
    SyncStdioPipeHandler* instance = new SyncStdioPipeHandler(child_fd, readable, writable, input_buffer);

    int r = uv_pipe_init(loop, instance->uv_pipe(), 0);
    if (r < 0) {
      delete instance;
      return r;
    }

    instance->uv_pipe()->data = instance;

    *instance_ptr = instance;
    return 0;
  }

  int Start() {
    assert(!busy_ && !closing_);

    // Set the busy flag already. If this function fails no recovery is
    // possible.
    busy_ = true;

    if (readable()) {
      if (input_buffer_.len > 0) {
        assert(input_buffer_.base != NULL);

        int r = uv_write(&write_req_, uv_stream(), &input_buffer_, 1, WriteCallback);
        if (r < 0)
          return r;
      }

      int r = uv_shutdown(&shutdown_req_, uv_stream(), ShutdownCallback);
      if (r < 0)
        return r;
    }

    if (writable()) {
      int r = uv_read_start(uv_stream(), AllocCallback, ReadCallback);
      if (r < 0)
        return r;
    }

    return 0;
  }

  void Close() {
    assert(!closing_);

    uv_close(uv_handle(), CloseCallback);

    closing_ = true;
  }

  size_t OutputLength() const {
    assert(!closing_);

    size_t size = 0;
    for (OutputBuffer* buf = first_buffer_; buf != NULL; buf = buf->next())
      size += buf->used();

    return size;
  }

  size_t CopyOutput(char* dest) const {
    assert(!closing_);

    size_t offset = 0;
    for (OutputBuffer* buf = first_buffer_; buf != NULL; buf = buf->next())
      offset += buf->Copy(dest + offset);

    return offset;
  }

  inline uv_pipe_t* uv_pipe() const {
    assert(!closing_);
    return &uv_pipe_;
  }

  inline uv_stream_t* uv_stream() const {
    return reinterpret_cast<uv_stream_t*>(uv_pipe());
  }

  inline uv_handle_t* uv_handle() const {
    return reinterpret_cast<uv_handle_t*>(uv_pipe());
  }

  inline int child_fd() const {
    assert(!closing_);
    return child_fd_;
  }

  inline bool readable() const {
    return readable_;
  }

  inline bool writable() const {
    return writable_;
  }

  inline ::uv_stdio_flags uv_stdio_flags() const {
    unsigned int flags;

    flags = UV_CREATE_PIPE;
    if (readable())
      flags |= UV_READABLE_PIPE;
    if (writable())
      flags |= UV_WRITABLE_PIPE;

    return static_cast<::uv_stdio_flags>(flags);
  }

  inline int status() const {
    assert(!closing_);
    return status_;
  }

  inline SyncStdioPipeHandler* next() const {
    return next_;
  }

  inline void set_next(SyncStdioPipeHandler* next) {
    next_ = next;
  }

 private:
  SyncStdioPipeHandler(int child_fd, bool readable, bool writable, uv_buf_t input_buffer):
      child_fd_(child_fd),
      readable_(readable),
      writable_(writable),
      input_buffer_(input_buffer),
      status_(0),
      first_buffer_(NULL),
      last_buffer_(NULL),
      busy_(false),
      closing_(false),
      next_(NULL) {
    assert(readable || writable);
  }

  inline ~SyncStdioPipeHandler() {
    OutputBuffer* buf;
    OutputBuffer* next;

    for (buf = first_buffer_; buf != NULL; buf = next) {
      next = buf->next();
      delete buf;
    }
  }

  inline uv_buf_t OnAlloc(size_t suggested_size) {
    // This function that libuv will never allocate two buffers for the same
    // stream at the same time. There's an assert in OutputBuffer::OnRead that
    // would fail if this assumption was violated.

    if (last_buffer_ == NULL) {
      // Allocate the first capture buffer.
      first_buffer_ = last_buffer_ = new OutputBuffer();

    } else if (last_buffer_->available() == 0) {
      // The current capture buffer is full so get us a new one.
      OutputBuffer* buf = new OutputBuffer();
      last_buffer_->set_next(buf);
      last_buffer_ = buf;
    }

    return last_buffer_->OnAlloc(suggested_size);
  }

  static uv_buf_t AllocCallback(uv_handle_t* handle, size_t suggested_size) {
    SyncStdioPipeHandler* self = reinterpret_cast<SyncStdioPipeHandler*>(handle->data);
    return self->OnAlloc(suggested_size);
  }

  void OnRead(uv_buf_t buf, ssize_t nread) {
    fprintf(stderr, "%d\n", (int) nread);

    if (nread == UV_EOF) {
      // Libuv implicitly stops reading on EOF.

    } else if (nread < 0) {
      SetError(static_cast<int>(nread));
      uv_read_stop(uv_stream());

    } else {
      last_buffer_->OnRead(buf, nread);
    }
  }

  static void ReadCallback(uv_stream_t* stream, ssize_t nread, uv_buf_t buf) {
    SyncStdioPipeHandler* self = reinterpret_cast<SyncStdioPipeHandler*>(stream->data);
    self->OnRead(buf, nread);
  }

  inline void OnWriteDone(int result) {
    if (result < 0)
      SetError(result);
  }

  static void WriteCallback(uv_write_t* req, int result) {
    SyncStdioPipeHandler* self = reinterpret_cast<SyncStdioPipeHandler*>(req->handle->data);
    self->OnWriteDone(result);
  }

  inline void OnShutdownDone(int result) {
    if (result < 0)
      SetError(result);
  }

  static void ShutdownCallback(uv_shutdown_t* req, int result) {
    SyncStdioPipeHandler* self = reinterpret_cast<SyncStdioPipeHandler*>(req->handle->data);
    self->OnShutdownDone(result);
  }

  inline void OnClose() {
    delete this;
  }

  static void CloseCallback(uv_handle_t* handle) {
    SyncStdioPipeHandler* self = reinterpret_cast<SyncStdioPipeHandler*>(handle->data);
    self->OnClose();
  }

  inline void SetError(int error) {
    if (status_ == 0)
      status_ = error;
  }

  class OutputBuffer {
    static const unsigned int kBufferSize = 65536;

   public:
    inline OutputBuffer(): used_(0), next_(NULL) {
    }

    inline uv_buf_t OnAlloc(size_t suggested_size) const {
      if (used() == kBufferSize)
        return uv_buf_init(NULL, 0);

      return uv_buf_init(data_ + used(), available());
    }

    inline void OnRead(uv_buf_t buf, size_t nread) {
      // If we hand out the same chunk twice, this should catch it.
      assert(buf.base == data_ + used());
      used_ += static_cast<unsigned int>(nread);
    }

    inline unsigned int available() const {
      return sizeof data_ - used();
    }

    inline unsigned int used() const {
      return used_;
    }

    inline size_t Copy(char* dest) const {
      memcpy(dest, data_, used());
      return used();
    }

    inline OutputBuffer* next() const {
      return next_;
    }

    inline void set_next(OutputBuffer* next) {
      next_ = next;
    }

   private:
    // use unsigned int because that's what uv_buf_init takes.
    mutable char data_[kBufferSize];
    unsigned int used_;

    OutputBuffer* next_;
  };


  mutable uv_pipe_t uv_pipe_;

  uv_buf_t input_buffer_;
  uv_write_t write_req_;
  uv_shutdown_t shutdown_req_;

  bool readable_;
  bool writable_;

  int child_fd_;
  int status_;

  SyncStdioPipeHandler* next_;

  OutputBuffer* first_buffer_;
  OutputBuffer* last_buffer_;

  bool busy_;
  bool closing_;
};


class ProcessWrap : public HandleWrap {
 public:
  static void Initialize(Handle<Object> target) {
    HandleScope scope(node_isolate);

    Local<FunctionTemplate> constructor = FunctionTemplate::New(New);
    constructor->InstanceTemplate()->SetInternalFieldCount(1);
    constructor->SetClassName(FIXED_ONE_BYTE_STRING(node_isolate, "Process"));

    NODE_SET_PROTOTYPE_METHOD(constructor, "close", HandleWrap::Close);

    NODE_SET_PROTOTYPE_METHOD(constructor, "spawn", Spawn);
    NODE_SET_PROTOTYPE_METHOD(constructor, "kill", Kill);

    NODE_SET_PROTOTYPE_METHOD(constructor, "ref", HandleWrap::Ref);
    NODE_SET_PROTOTYPE_METHOD(constructor, "unref", HandleWrap::Unref);

    NODE_SET_METHOD(target, "spawnSync", SpawnSync);

    target->Set(FIXED_ONE_BYTE_STRING(node_isolate, "Process"),
                constructor->GetFunction());
  }

 private:
  static void New(const FunctionCallbackInfo<Value>& args) {
    // This constructor should not be exposed to public javascript.
    // Therefore we assert that we are not trying to call this as a
    // normal function.
    assert(args.IsConstructCall());
    HandleScope scope(node_isolate);
    new ProcessWrap(args.This());
  }

  explicit ProcessWrap(Handle<Object> object)
      : HandleWrap(object, reinterpret_cast<uv_handle_t*>(&process_)) {
  }

  ~ProcessWrap() {
  }

  static void ParseStdioOptions(Local<Object> js_options,
                                uv_process_options_t* options) {
    Local<String> stdio_key =
        FIXED_ONE_BYTE_STRING(node_isolate, "stdio");
    Local<Array> stdios = js_options->Get(stdio_key).As<Array>();
    uint32_t len = stdios->Length();
    options->stdio = new uv_stdio_container_t[len];
    options->stdio_count = len;

    for (uint32_t i = 0; i < len; i++) {
      Local<Object> stdio = stdios->Get(i).As<Object>();
      Local<Value> type =
          stdio->Get(FIXED_ONE_BYTE_STRING(node_isolate, "type"));

      if (type->Equals(FIXED_ONE_BYTE_STRING(node_isolate, "ignore"))) {
        options->stdio[i].flags = UV_IGNORE;

      } else if (type->Equals(FIXED_ONE_BYTE_STRING(node_isolate, "pipe"))) {
        options->stdio[i].flags = static_cast<uv_stdio_flags>(
            UV_CREATE_PIPE | UV_READABLE_PIPE | UV_WRITABLE_PIPE);
        Local<String> handle_key =
            FIXED_ONE_BYTE_STRING(node_isolate, "handle");
        Local<Object> handle = stdio->Get(handle_key).As<Object>();
        options->stdio[i].data.stream =
            reinterpret_cast<uv_stream_t*>(
                PipeWrap::Unwrap(handle)->UVHandle());
      } else if (type->Equals(FIXED_ONE_BYTE_STRING(node_isolate, "wrap"))) {
        Local<String> handle_key =
            FIXED_ONE_BYTE_STRING(node_isolate, "handle");
        Local<Object> handle = stdio->Get(handle_key).As<Object>();
        uv_stream_t* stream = HandleToStream(handle);
        assert(stream != NULL);

        options->stdio[i].flags = UV_INHERIT_STREAM;
        options->stdio[i].data.stream = stream;
      } else {
        Local<String> fd_key = FIXED_ONE_BYTE_STRING(node_isolate, "fd");
        int fd = static_cast<int>(stdio->Get(fd_key)->IntegerValue());

        options->stdio[i].flags = UV_INHERIT_FD;
        options->stdio[i].data.fd = fd;
      }
    }
  }

  static int ParseSyncStdioOptions(uv_loop_t* loop,
                                   Local<Object> js_options,
                                   uv_process_options_t* options,
                                   SyncStdioPipeHandler** pipe_handlers_head_ptr) {
    int err = 0;

    SyncStdioPipeHandler* pipe_handlers_head = NULL;
    SyncStdioPipeHandler* pipe_handlers_tail = NULL;

    Local<String> stdio_key =
        FIXED_ONE_BYTE_STRING(node_isolate, "stdio");
    Local<Array> stdios = js_options->Get(stdio_key).As<Array>();
    uint32_t len = stdios->Length();

    options->stdio = new uv_stdio_container_t[len];
    options->stdio_count = len;

    for (uint32_t i = 0; i < len; i++) {
      Local<Object> stdio = stdios->Get(i).As<Object>();
      Local<Value> type =
          stdio->Get(FIXED_ONE_BYTE_STRING(node_isolate, "type"));

      if (type->Equals(FIXED_ONE_BYTE_STRING(node_isolate, "ignore"))) {
        options->stdio[i].flags = UV_IGNORE;

      } else if (type->Equals(FIXED_ONE_BYTE_STRING(node_isolate, "pipe"))) {
        bool readable = stdio->Get(FIXED_ONE_BYTE_STRING(node_isolate, "readable"))->BooleanValue();
        bool writable = stdio->Get(FIXED_ONE_BYTE_STRING(node_isolate, "writable"))->BooleanValue();

        uv_buf_t buf = uv_buf_init(NULL, 0);

        if (readable) {
          Local<Value> input = stdio->Get(FIXED_ONE_BYTE_STRING(node_isolate, "input"));
          if (!Buffer::HasInstance(input))
            // We can only deal with buffers for now.
            assert(input->IsUndefined());
          else
            buf = uv_buf_init(Buffer::Data(input),
                              static_cast<unsigned int>(Buffer::Length(input)));

        }

        SyncStdioPipeHandler* pipe_handler;
        err = SyncStdioPipeHandler::New(loop, i, readable, writable, buf, &pipe_handler);
        if (err < 0)
          goto error;

        if (pipe_handlers_head == NULL)
          pipe_handlers_head = pipe_handler;
        else
          pipe_handlers_tail->set_next(pipe_handler);

        pipe_handlers_tail = pipe_handler;

        options->stdio[i].flags = pipe_handler->uv_stdio_flags();
        options->stdio[i].data.stream = pipe_handler->uv_stream();

      } else if (type->Equals(FIXED_ONE_BYTE_STRING(node_isolate, "wrap"))) {
        Local<String> handle_key =
            FIXED_ONE_BYTE_STRING(node_isolate, "handle");
        Local<Object> handle = stdio->Get(handle_key).As<Object>();
        uv_stream_t* stream = HandleToStream(handle);
        assert(stream != NULL);

        options->stdio[i].flags = UV_INHERIT_STREAM;
        options->stdio[i].data.stream = stream;

      } else {
        Local<String> fd_key = FIXED_ONE_BYTE_STRING(node_isolate, "fd");
        int fd = static_cast<int>(stdio->Get(fd_key)->IntegerValue());

        options->stdio[i].flags = UV_INHERIT_FD;
        options->stdio[i].data.fd = fd;
      }
    }

    *pipe_handlers_head_ptr = pipe_handlers_head;

    return 0;

   error:
    for (SyncStdioPipeHandler* h = pipe_handlers_head; h != NULL; h = h->next())
      h->Close();

    return err;
  }


  static int ParseMiscOptions(Handle<Object> js_options,
                          uv_process_options_t* options) {
    HandleScope scope(node_isolate);

    // options.uid
    Local<Value> uid_v =
        js_options->Get(FIXED_ONE_BYTE_STRING(node_isolate, "uid"));
    if (uid_v->IsInt32()) {
      int32_t uid = uid_v->Int32Value();
      if (uid & ~((uv_uid_t) ~0)) {
        ThrowRangeError("options.uid is out of range");
        return UV_EINVAL;
      }
      options->flags |= UV_PROCESS_SETUID;
      options->uid = (uv_uid_t) uid;
    } else if (!uid_v->IsUndefined() && !uid_v->IsNull()) {
      ThrowTypeError("options.uid should be a number");
      return UV_EINVAL;
    }

    // options->gid
    Local<Value> gid_v =
        js_options->Get(FIXED_ONE_BYTE_STRING(node_isolate, "gid"));
    if (gid_v->IsInt32()) {
      int32_t gid = gid_v->Int32Value();
      if (gid & ~((uv_gid_t) ~0)) {
        ThrowRangeError("options.gid is out of range");
        return UV_EINVAL;
      }
      options->flags |= UV_PROCESS_SETGID;
      options->gid = (uv_gid_t) gid;
    } else if (!gid_v->IsUndefined() && !gid_v->IsNull()) {
      ThrowTypeError("options.gid should be a number");
      return UV_EINVAL;
    }

    // TODO(bnoordhuis) is this possible to do without mallocing ?

    // options->file
    Local<Value> file_v =
        js_options->Get(FIXED_ONE_BYTE_STRING(node_isolate, "file"));
    String::Utf8Value file(file_v->IsString() ? file_v : Local<Value>());
    if (file.length() > 0) {
      options->file = strdup(*file);
    } else {
      ThrowTypeError("Bad argument");
      return UV_EINVAL;
    }

    // options->args
    Local<Value> argv_v =
        js_options->Get(FIXED_ONE_BYTE_STRING(node_isolate, "args"));
    if (!argv_v.IsEmpty() && argv_v->IsArray()) {
      Local<Array> js_argv = Local<Array>::Cast(argv_v);
      int argc = js_argv->Length();
      // Heap allocate to detect errors. +1 is for NULL.
      options->args = new char*[argc + 1];
      for (int i = 0; i < argc; i++) {
        String::Utf8Value arg(js_argv->Get(i));
        options->args[i] = strdup(*arg);
      }
      options->args[argc] = NULL;
    }

    // options->cwd
    Local<Value> cwd_v =
        js_options->Get(FIXED_ONE_BYTE_STRING(node_isolate, "cwd"));
    String::Utf8Value cwd(cwd_v->IsString() ? cwd_v : Local<Value>());
    if (cwd.length() > 0) {
      options->cwd = *cwd;
    }

    // options->env
    Local<Value> env_v =
        js_options->Get(FIXED_ONE_BYTE_STRING(node_isolate, "envPairs"));
    if (!env_v.IsEmpty() && env_v->IsArray()) {
      Local<Array> env = Local<Array>::Cast(env_v);
      int envc = env->Length();
      options->env = new char*[envc + 1]; // Heap allocated to detect errors.
      for (int i = 0; i < envc; i++) {
        String::Utf8Value pair(env->Get(i));
        options->env[i] = strdup(*pair);
      }
      options->env[envc] = NULL;
    }

    // options->winfs_verbatim_arguments
    Local<String> windows_verbatim_arguments_key =
        FIXED_ONE_BYTE_STRING(node_isolate, "windowsVerbatimArguments");
    if (js_options->Get(windows_verbatim_arguments_key)->IsTrue()) {
      options->flags |= UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS;
    }

    // options->detached
    Local<String> detached_key =
        FIXED_ONE_BYTE_STRING(node_isolate, "detached");
    if (js_options->Get(detached_key)->IsTrue()) {
      options->flags |= UV_PROCESS_DETACHED;
    }

    return 0;
  }

  static void CleanupOptions(uv_process_options_t* options) {
    free(const_cast<char*>(options->file));

    if (options->args) {
      for (int i = 0; options->args[i]; i++)
        free(options->args[i]);
      delete [] options->args;
    }

    if (options->env) {
      for (int i = 0; options->env[i]; i++)
        free(options->env[i]);
      delete [] options->env;
    }

    delete[] options->stdio;
  }


  static void SpawnSync(const FunctionCallbackInfo<Value>& args) {
    HandleScope scope(node_isolate);

    int r = 0;
    int r2;

    uv_loop_t* loop = NULL;

    uv_process_options_t options;
    memset(&options, 0, sizeof options);

    SyncStdioPipeHandler* pipe_handlers;

    uv_process_t process;

    assert(args[0]->IsObject());
    Local<Object> js_options = args[0].As<Object>();

    Local<Array> stdio_output = Array::New();

    loop = uv_loop_new();
    if (loop == NULL) {
      r = UV_ENOMEM;
      goto out1;
    }

    r = ParseMiscOptions(js_options, &options);
    if (r < 0)
      goto out2;

    r = ParseSyncStdioOptions(loop, js_options, &options, &pipe_handlers);
    if (r < 0)
      goto out3;

    r = uv_spawn(loop, &process, options);
    if (r < 0)
      goto out4;

    // Start reading and writing on the stdio pipes.
    for (SyncStdioPipeHandler* h = pipe_handlers; h != NULL; h = h->next())
      h->Start();

    r = uv_run(loop, UV_RUN_DEFAULT);

    if (r >= 0) {
      // Pick up the exit code from the process.
    }

    // Since uv_run has returned, all pipes are done reading and writing. Pick up any output and exit stats.
    for (SyncStdioPipeHandler* h = pipe_handlers; h != NULL; h = h->next()) {
      size_t output_length = h->OutputLength();
      printf("%d, length: %d\n", h->child_fd(), (int) output_length);
      Local<Object> js_buffer = Buffer::New(output_length);
      h->CopyOutput(Buffer::Data(js_buffer));
      stdio_output->Set(h->child_fd(), js_buffer);
    }

   out4:
    // Close all stdio pipes.
    for (SyncStdioPipeHandler* h = pipe_handlers; h != NULL; h = h->next())
      h->Close();

   out3:
    r2 = uv_run(loop, UV_RUN_DEFAULT);
    if (r2 < 0 && r >= 0)
      r = r2;

    CleanupOptions(&options);

   out2:
    uv_loop_delete(loop);

   out1:
    Local<Object> js_result = Object::New();
    js_result->Set(FIXED_ONE_BYTE_STRING(node_isolate, "result"), Integer::New(r, node_isolate));
    js_result->Set(FIXED_ONE_BYTE_STRING(node_isolate, "output"), stdio_output);

    args.GetReturnValue().Set(js_result);

    printf("returning!\n");
  }

  static void Spawn(const FunctionCallbackInfo<Value>& args) {
    HandleScope scope(node_isolate);
    ProcessWrap* wrap;

    NODE_UNWRAP(args.This(), ProcessWrap, wrap);

    Local<Object> js_options = args[0]->ToObject();

    uv_process_options_t options;
    memset(&options, 0, sizeof(uv_process_options_t));

    {
      TryCatch try_catch;

      ParseMiscOptions(js_options, &options);

      if (try_catch.HasCaught()) {
        try_catch.ReThrow();
        return;
      }
    }

    ParseStdioOptions(js_options, &options);

    options.exit_cb = OnExit;

    int err = uv_spawn(uv_default_loop(), &wrap->process_, options);

    if (err == 0) {
      assert(wrap->process_.data == wrap);
      wrap->object()->Set(FIXED_ONE_BYTE_STRING(node_isolate, "pid"),
                          Integer::New(wrap->process_.pid, node_isolate));
    }

    CleanupOptions(&options);

    args.GetReturnValue().Set(err);
  }

  static void Kill(const FunctionCallbackInfo<Value>& args) {
    HandleScope scope(node_isolate);
    ProcessWrap* wrap;
    NODE_UNWRAP(args.This(), ProcessWrap, wrap);

    int signal = args[0]->Int32Value();
    int err = uv_process_kill(&wrap->process_, signal);
    args.GetReturnValue().Set(err);
  }

  static void OnExit(uv_process_t* handle,
                     int64_t exit_status,
                     int term_signal) {
    HandleScope scope(node_isolate);

    ProcessWrap* wrap = static_cast<ProcessWrap*>(handle->data);
    assert(wrap);
    assert(&wrap->process_ == handle);

    Local<Value> argv[] = {
      Number::New(node_isolate, static_cast<double>(exit_status)),
      OneByteString(node_isolate, signo_string(term_signal))
    };

    if (onexit_sym.IsEmpty()) {
      onexit_sym = FIXED_ONE_BYTE_STRING(node_isolate, "onexit");
    }

    MakeCallback(wrap->object(), onexit_sym, ARRAY_SIZE(argv), argv);
  }

  uv_process_t process_;
};


}  // namespace node

NODE_MODULE(node_process_wrap, node::ProcessWrap::Initialize)
