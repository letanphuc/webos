TARGET := rust_hello.wasm
RUST_TARGET ?= wasm32-unknown-unknown
CARGO ?= cargo

.PHONY: all clean

all: $(TARGET)

$(TARGET): Cargo.toml src/main.rs
	RUSTFLAGS="$(RUSTFLAGS) -C link-arg=-zstack-size=4096 -C link-arg=--initial-memory=65536" \
		$(CARGO) build --release --target $(RUST_TARGET)
	cp target/$(RUST_TARGET)/release/rust_hello.wasm $@

clean:
	$(CARGO) clean
	rm -f $(TARGET)
