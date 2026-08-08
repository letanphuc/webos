TARGET := conformance_rust.wasm
RUST_TARGET ?= wasm32-unknown-unknown
all:
	RUSTFLAGS="-C link-arg=-zstack-size=4096 -C link-arg=--initial-memory=65536" cargo build --release --target $(RUST_TARGET)
	cp target/$(RUST_TARGET)/release/webos_abi_conformance.wasm $(TARGET)
clean:
	cargo clean
	rm -f $(TARGET)
