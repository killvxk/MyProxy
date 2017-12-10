#pragma once
#include "basic.h"
#include <atomic>

namespace MyProxy {

	template <typename Protocol>
	class AbstractProxySession : 
		public BasicProxySession, 
		public std::enable_shared_from_this<AbstractProxySession<Protocol>>
	{
	public:
		template <typename Protocol>
		struct TraitsProtoType;
		template<>
		struct TraitsProtoType<boost::asio::ip::tcp> {
			static constexpr ProtoType type = ProtoType::Tcp;
		};
		template<>
		struct TraitsProtoType<boost::asio::ip::udp> {
			static constexpr ProtoType type = ProtoType::Udp;
		};

		AbstractProxySession(SessionId id, boost::asio::io_service &io, std::string loggerName = "Session") :
			BasicProxySession(id,io, loggerName),
			m_socket(io), m_writeStrand(io) {}
		virtual ~AbstractProxySession() {
			//std::cout << "~AbstractProxySession\n";
		}
		//Get m_socket.
		typename Protocol::socket& socket() { return m_socket; }
		virtual void start() = 0;
		virtual void stop() override;
		//Stop and remove self from session manager.
		virtual void destroy();
	protected:
		//Read socket and write to tunnel.
		virtual void startForwarding();
		virtual void startForwarding_impl();
		//Write to socket.
		void write(std::shared_ptr<DataVec> dataPtr);
		void write_impl();
		void write_handler(const boost::system::error_code &ec, size_t bytes, std::shared_ptr<BasicProxySession> self);
		//Get parent tunnel.
		std::atomic<bool> _running = false;
	private:
		typename Protocol::socket m_socket;
		boost::asio::strand m_writeStrand;
		std::queue<std::shared_ptr<DataVec>> m_writeQueue;
		//std::array<char,1024> m_readBuffer;
		boost::asio::streambuf m_readBuffer2;
	};

	template<typename Protocol>
	inline void AbstractProxySession<Protocol>::stop()
	{
		if (m_socket.is_open()) {
			boost::system::error_code ec;
			m_socket.shutdown(m_socket.shutdown_both, ec);
			if (ec) {
				logger()->debug("ID: {} Shutdown error: {}",id(), ec.message());
			}
			ec.clear();
			m_socket.close(ec);
			if (ec) {
				logger()->debug("ID: {} Close error: {}",id(), ec.message());
			}
		}
	}

	template<typename Protocol>
	inline void AbstractProxySession<Protocol>::destroy()
	{
		if (_running.exchange(false)) {
			auto sessionId = this->id();
			if (tunnel()->manager().remove(sessionId)) {
				tunnel()->sessionDestroyNotify(sessionId);
			}
		}
	}

	template<typename Protocol>
	inline void AbstractProxySession<Protocol>::startForwarding()
	{
		_running.store(true);
		this->onReceived = [this](std::shared_ptr<SessionPackage> package) {
			if(_running.load()) //...
				write(std::make_shared<DataVec>(std::move(package->data)));
		};
		//m_readBuffer.fill('\0');
		startForwarding_impl();
	}

	template<typename Protocol>
	inline void AbstractProxySession<Protocol>::startForwarding_impl()
	{
		using namespace boost::asio;
		m_socket.async_receive(m_readBuffer2.prepare(1024 * 4), [this, self = shared_from_this()](const boost::system::error_code &ec, size_t bytes) {
			if (ec) {
				if (!_running.load())
					return;
				logger()->debug("ID: {} Destroy, reason: {}", id(), ec.message());
				destroy();
				return;
			}
			auto data = buffer_cast<const char*>(m_readBuffer2.data());
			SessionPackage package{ id(),DataVec{ data, data + bytes } };
			tunnel()->write(std::make_shared<DataVec>(package.toDataVec()));
			m_readBuffer2.consume(bytes);
			startForwarding_impl();
		});
	}

	template<typename Protocol>
	inline void AbstractProxySession<Protocol>::write(std::shared_ptr<DataVec> dataPtr)
	{
		//logger()->trace("AbstractProxySession<Protocol>::write() {} bytes write method posted", dataPtr->size());
		m_writeStrand.post([this, dataPtr = std::move(dataPtr), self = shared_from_this()]{
			if (!_running.load())
				return;
			//logger()->trace("AbstractProxySession<Protocol>::write() -> Lambda: {} bytes push to m_writeQueue", dataPtr->size());
			m_writeQueue.push(std::move(dataPtr));
			if (m_writeQueue.size() > 1) {
				return;
			}
			else {
				//m_writeStrand.post(std::bind(&AbstractProxySession<Protocol>::write_impl, this));
				write_impl();
			}
		});
	}

	template<>
	inline void AbstractProxySession<boost::asio::ip::tcp>::write_impl()
	{
		if (m_writeQueue.empty()) {
			//logger()->trace("AbstractProxySession<boost::asio::ip::tcp>::write_impl() m_writeQueue empty");
			return;
		}
		//logger()->trace("AbstractProxySession<boost::asio::ip::tcp>::write_impl() {} bytes write method start async_write", m_writeQueue.front()->size());
		async_write(m_socket, boost::asio::buffer(*m_writeQueue.front()), boost::asio::transfer_all(),
			m_writeStrand.wrap(std::bind(&MyProxy::AbstractProxySession<boost::asio::ip::tcp>::write_handler,this,std::placeholders::_1,std::placeholders::_2, shared_from_this())));
	}

	template<typename Protocol>
	inline void MyProxy::AbstractProxySession<Protocol>::write_handler(const boost::system::error_code & ec, size_t bytes, std::shared_ptr<BasicProxySession>)
	{
		m_writeQueue.pop(); //drop
		if (ec) {
			if (!_running.load())
				return;
			logger()->debug("ID: {} write error: {}", id(), ec.message());
			destroy();
			return;
		}
		if (!m_writeQueue.empty()) {
			//m_writeStrand.post(std::bind(&AbstractProxySession<boost::asio::ip::tcp>::write_impl, this));
			write_impl();
		}
	}

	template<>
	inline void AbstractProxySession<boost::asio::ip::udp>::write_impl()
	{
		//if (m_writeQueue.size() == 0) {
		//	return;
		//}
		m_socket.async_send(boost::asio::buffer(*m_writeQueue.front()),
		m_writeStrand.wrap([this, self = shared_from_this()](const boost::system::error_code &ec, size_t) {
			m_writeQueue.pop();
			if (ec) {
				if (!_running.load())
					return;
				logger()->debug("ID: {} write error: {}", id(), ec.message());
				destroy();
				return;
			}
			m_writeQueue.pop();
			if (!m_writeQueue.empty()) {
				//m_writeStrand.post(std::bind(&AbstractProxySession<boost::asio::ip::udp>::write_impl, this));
				write_impl();
			}
		}));
	}
}