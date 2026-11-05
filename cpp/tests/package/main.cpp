#include <fanuc_snpx/client.hpp>
#include <fanuc_snpx/fanuc_driver.hpp>
#include <robot/robot_driver.hpp>

int main() {
    std::unique_ptr<robot::RobotDriver> driver = std::make_unique<fanuc::FanucDriver>();
    auto bits = driver->read_bits(robot::Port::digital_output, 0);
    if (bits || bits.error().code != robot::Error::Code::disconnected) return 1;
    fanuc::snpx::Client client;
    auto result = client.read(fanuc::snpx::digital_output, 1).get();
    return !result && result.error().code == fanuc::snpx::Error::Code::disconnected ? 0 : 1;
}
