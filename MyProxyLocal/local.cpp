#include "local.h"
#include <time.h>

using namespace boost::asio;
//using boost::asio::detail::socket_ops::*;

namespace MyProxy {

	namespace Local {
		bool LocalProxyTunnel::tls_session_established(const Botan::TLS::Session & session)
		{
			if (onReady) {
				logger()->debug("Ready notify");
				onReady();
			}
			return true;
		}
		void LocalProxyTunnel::handleRead(std::shared_ptr<DataVec> data)
		{
			//if (!_running.load()) {
			//	logger()->warn("handleRead() cancel: tunnel stoped");
			//	return;
			//}
			auto type = static_cast<Package::Type>(data->at(0));
			if (type == Package::Type::Session) {
				auto package = std::make_shared<SessionPackage>();
				IoHelper(*data) >> *package;
				dispatch(package);
			}
			else if (type == Package::Type::Tunnel) {
				TunnelMethod method = TunnelPackage::getTunnelMethod(data->data());
				if (method == TunnelMethod::SessionDestroy) {
					SessionId sessionId;
					std::tie(std::ignore, std::ignore, std::ignore, sessionId) = IoHelper(*data).getTuple<Package::Type, Package::SizeType, TunnelMethod, SessionId>(_1B, _4B, _1B, _4B);
					auto session = manager().get(sessionId);
					if (session)
						session->destroy(true);
				}
				else {
					logger()->error("Unknown tunnel method");
					disconnect();
					return;
				}
			}
			else {
				//auto header = *buffer_cast<const uint64_t*>(readbuf().data());
				logger()->error("Unknown package received");
				disconnect();
				return;
			}
		}

		Local::Local(boost::asio::io_context &io): _io(io),m_work(io.get_executor()), m_resolver(io), m_timer(io)
		{
			auto rng = std::make_unique<Botan::AutoSeeded_RNG>();
			auto mgr = std::make_unique<Botan::TLS::Session_Manager_In_Memory>(*rng);
			auto creds = std::make_unique<MyProxy::Credentials>("tls-client", *rng);
			auto policy = std::make_unique<MyProxy::Policy>();
			_ctx = std::make_unique<TLSContext>
				(std::move(rng), std::move(mgr), std::move(creds), std::move(policy));
		}
		Local::~Local()
		{
			spdlog::drop("Local");
		}
		void Local::setServer(std::string host, std::string port)
		{
			m_serverHost = host;
			m_serverPort = port;
		}

		void Local::setCA(std::string path)
		{
			_ctx->creds->addCA(path);
		}

		void Local::setCertAndKey(std::string certPath, std::string keyPath)
		{
			_ctx->creds->addPair(certPath, keyPath);
		}

		void Local::bind(std::string port, std::string bindAddress)
		{
			ip::tcp::endpoint bindEp;
			if (bindAddress.size() == 0) {
				bindEp = ip::tcp::endpoint(ip::tcp::v4(), std::stoi(port));
			}
			else {
				bindEp = ip::tcp::endpoint(ip::address::from_string(bindAddress), std::stoi(port));
			}
			m_logger->info("Listen at: {}:{}", bindEp.address().to_string(), bindEp.port());
			boost::system::error_code ec;
			m_tcpAcceptor.reset(new ip::tcp::acceptor(_io, bindEp));
		}

		void Local::start()
		{
			auto tunnel = std::make_shared<LocalProxyTunnel>(*_ctx, _io);
			tunnel->onDisconnected = [this] {
				//_tunnelAvailable.store(false);
				std::unique_lock<std::shared_mutex> locker(tunnelMutex);
				m_tunnel.reset();
				locker.unlock();
				m_logger->info("Local restart");
				start();
			};
			//locker.unlock();
			m_resolver.async_resolve(m_serverHost, m_serverPort, [this, tunnel](const boost::system::error_code &ec, ip::tcp::resolver::results_type result) {
				if (ec) {
					m_logger->error("Resolve failed: {}", ec.message());
					//retry?
					return;
				}
				m_logger->info("{} resolved to {}", m_serverHost,(*result).endpoint().address().to_string());
				startConnect(tunnel, result);
			});
		}

		void Local::startAccept()
		{
			auto session = std::make_shared<LocalProxySession<ip::tcp>>(newSessionId(), _io);
			m_tcpAcceptor->async_accept(session->socket(), [this, session = session](const boost::system::error_code &ec) {
				std::shared_lock<std::shared_mutex> locker(tunnelMutex);
				if (!m_tunnel) {
					m_logger->warn("Tunnel not available");
					return;
				}
				if (!ec) {
					auto ep = session->socket().remote_endpoint();
					m_logger->debug("New Session ID: {} from {}:{}", session->id(), ep.address().to_string(), ep.port());
					session->setTunnel(m_tunnel);
					m_tunnel->manager().insertAndStart(session);
				} else {
					m_logger->debug("async_accept error: ", ec.message());
				}
				startAccept();
			});
		}

		void Local::startConnect(std::shared_ptr<LocalProxyTunnel> tunnel, ip::tcp::resolver::results_type result)
		{
			async_connect(tunnel->connection(), result, [this, tunnel, result](const boost::system::error_code &ec, ip::tcp::endpoint ep) {
				if (ec) {
					m_logger->error("Connectd failed: {}", ec.message());
					m_timer.expires_from_now(boost::posix_time::seconds(5));
					m_timer.async_wait([this, tunnel, result](const boost::system::error_code &ec) {
						if (ec) {
							m_logger->warn("Timer error: {}",ec.message());
							return;
						}
						startConnect(tunnel, result);
					});
					return;
				}
				tunnel->connection().set_option(ip::tcp::no_delay(true));
				//auto ep = (*result).endpoint();
				m_logger->info("Connectd to server: {}:{}", ep.address().to_string(), ep.port());
				std::unique_lock<std::shared_mutex> locker(tunnelMutex);
				m_tunnel = tunnel;
				m_tunnel->onReady = std::bind(&Local::startAccept, this);
				m_tunnel->start();
				//startAccept();
			});
		}

		SessionId Local::newSessionId()
		{
			return (m_maxSessionId == sessionIdMax ? (m_maxSessionId = 0) : m_maxSessionId++);
		}

		DataVec parseHost(AddrType type, const DataVec &vec)
		{
			constexpr int maxlen = boost::asio::detail::max_addr_v6_str_len;
			std::vector<char> buf(maxlen);
			int af;
			switch (type)
			{
			case MyProxy::IPV4:
				af = AF_INET;
				break;
			case MyProxy::Domain:
				af = -1;
				break;
			case MyProxy::IPV6:
				af = AF_INET6;
				break;
			default:
				throw std::runtime_error("parseHost: error type");
				break;
			}
			boost::system::error_code ec;
			switch (type)
			{
			case MyProxy::IPV4:
			case MyProxy::IPV6:
				boost::asio::detail::socket_ops::inet_ntop(af, vec.data(), buf.data(), maxlen, 0, ec);
				if (ec) {
					throw ec;
				}
				break;
			case MyProxy::Domain:
				return vec;
				break;
			}
			buf.shrink_to_fit();
			return buf;
		}
}
}
