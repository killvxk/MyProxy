#include "local.h"
#include "server.h"
#include "sslsetting.h"

auto opensslLogger = spdlog::stdout_color_mt("OPENSSL");

int main() {

	using namespace std;

	MyProxy::openssl_config::thread_setup();

	//spd::set_async_mode(4096);
	spdlog::set_pattern("[%D %H:%M:%e] [%L] [%t] [%n]\t%v");
	spdlog::set_level(spdlog::level::info);
	//auto f = std::make_shared<spdlog::pattern_formatter>("[%D %H:%M:%e]\t[%L]\t[%n]\t%v");
	//spdlog::set_formatter(f);

	boost::asio::io_service io;

	MyProxy::Local::Local local(io);
	local.bind("1083");
	local.setServer("localhost", "1084");
	local.start();

	MyProxy::Server::Server server(io);
	server.bind("1084");
	server.setCert("E:\\pki\\servercert.pem");
	server.setKey("E:\\pki\\serverkey.pem");
	server.start();

	//io.run();

	auto threadLogger = spdlog::stdout_logger_mt("IO Thread");
	threadLogger->set_pattern("[%D %H:%M:%e] [%L] [%t] [%n]\t%v");
	threadLogger->set_level(spdlog::level::debug);

	std::vector<std::thread> threads;
	for (size_t i = 0; i < std::thread::hardware_concurrency(); i++) {
		threads.push_back(std::thread([&io, threadLogger] {
			spdlog::get("IO Thread")->info("Io service running!");
			try {
				io.run();
			}
			catch (std::exception ex) {
				spdlog::get("IO Thread")->error("Catch exception: ",ex.what());
				throw;
			}
		}));
	}

	for (auto &t : threads) {
		t.join();
	}

	MyProxy::openssl_config::thread_cleanup();

	::system("pause");

	return 0;

}