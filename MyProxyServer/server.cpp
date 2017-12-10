#include "server.h"

using namespace boost::asio;

namespace MyProxy {
	namespace Server {
		void ServerProxyTunnel::handshake()
		{
			socket().async_handshake(ssl::stream_base::server, [this, self = shared_from_this()](const boost::system::error_code &ec){
				if (ec) {
					logger()->error("handshake() error: {}", ec.message());
					disconnect();
					return;
				}
				logger()->debug("handshake success");
				startProcess();
			});
		}
		void ServerProxyTunnel::handleRead(const boost::system::error_code & ec, size_t bytes, std::shared_ptr<BasicProxyTunnel>)
		{
			if (ec) {
				logger()->error("handleRead() error: {}", ec.message());
				disconnect();
				return;
			}
			if (bytes == 0) {
				logger()->warn("handleRead() error: read zero bytes");
				nextRead();
				return;
			}
			auto data = buffer_cast<const char*>(readbuf().data());
			auto type = static_cast<Package::Type>(data[0]);
			if (type == Package::Type::Session) {
				auto package = std::make_shared<SessionPackage>();
				IoHelper(&readbuf()) >> *package;
				dispatch(package);
			}
			else if (type == Package::Type::Tunnel) {
				TunnelMethod method = TunnelPackage::getTunnelMethod(buffer_cast<const char*>(readbuf().data()));
				if (method == TunnelMethod::NewSession) {
					NewSessionRequest request;
					IoHelper(&readbuf()) >> request;
					std::shared_ptr<BasicProxySession> session;
					if (request.protoType == ProtoType::Tcp) {
						session = std::make_shared<ServerProxySession<ip::tcp>>(std::move(request), service());
					}
					else {
						session = std::make_shared<ServerProxySession<ip::udp>>(std::move(request), service());
					}
					session->setTunnel(shared_from_this());
					manager().insertAndStart(session);
				}
				else if (method == TunnelMethod::SessionDestroy) {
					SessionId sessionId;
					std::tie(std::ignore, std::ignore, std::ignore, sessionId) = IoHelper(&readbuf()).getTuple<Package::Type, Package::SizeType, TunnelMethod, SessionId>(_1B, _4B, _1B, _4B);
					manager().remove(sessionId);
				}
			}
			else {
				auto header = *buffer_cast<const uint64_t*>(readbuf().data());
				logger()->error("Unknown package received, package head: {0:x}", header);
				disconnect();
			}
			nextRead();
		}

		Server::Server(boost::asio::io_service &io):m_work(io)
		{
			m_ctx.set_verify_mode(m_ctx.verify_none);
		}

		Server::~Server()
		{
			spdlog::drop("Server");
		}

		void Server::setCert(std::string path)
		{
			m_ctx.use_certificate_file(path, m_ctx.pem);
		}

		void Server::setKey(std::string path)
		{
			m_ctx.use_private_key_file(path, m_ctx.pem);
		}

		void Server::bind(std::string port, std::string bindAddress)
		{
			ip::tcp::endpoint bindEp;
			if (bindAddress.size() == 0) {
				bindEp = ip::tcp::endpoint(ip::tcp::v4(), std::stoi(port));
			}
			else {
				bindEp = ip::tcp::endpoint(ip::address::from_string(bindAddress), std::stoi(port));
			}
			m_logger->info("Linsten at {}:{}", bindEp.address().to_string(), bindEp.port());
			boost::system::error_code ec;
			m_tcpAcceptor.reset(new ip::tcp::acceptor(m_work.get_io_service(), bindEp));
		}

		void Server::start()
		{
			startAccept();
		}

		void Server::startAccept()
		{
			auto tunnel = std::make_shared<ServerProxyTunnel>(m_work.get_io_service(), m_ctx);
			tunnel->onDisconnected = std::bind(&Server::startAccept, this);
			m_logger->info("Start accept");
			m_tcpAcceptor->async_accept(tunnel->connection(), [this, tunnel](const boost::system::error_code &ec) {
				if (!ec) {
					auto ep = tunnel->connection().remote_endpoint();
					m_logger->info("New tunnel connection from {}:{}",ep.address().to_string(),ep.port());
					//tunnelSet.insert(tunnel);
					tunnel->start();
					startAccept();
				}
			});
		}
	}
}