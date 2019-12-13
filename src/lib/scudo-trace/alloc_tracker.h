namespace scudo_trace {

class AllocTracker {
 public:
  explicit AllocTracker(fbl::Array<uint8_t> buffer) : data_(buffer) {}
 private:
  fbl::Array<uint8_t> data_;
};

}  // namespace scudo_trace
