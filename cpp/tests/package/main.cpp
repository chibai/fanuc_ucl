#include <fanuc_snpx/client.hpp>

int main() {
    fanuc::snpx::Client client;
    auto result = client.read(fanuc::snpx::digital_output, 1).get();
    return !result && result.error().code == fanuc::snpx::Error::Code::disconnected ? 0 : 1;
}
