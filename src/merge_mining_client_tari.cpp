/*
 * This file is part of the Monero P2Pool <https://github.com/SChernykh/p2pool>
 * Copyright (c) 2021-2024 SChernykh <https://github.com/SChernykh>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"
#include "merge_mining_client.h"
#include "merge_mining_client_tari.h"
#include "p2pool.h"
#include "params.h"

LOG_CATEGORY(MergeMiningClientTari)

using namespace tari::rpc;

namespace p2pool {

MergeMiningClientTari::MergeMiningClientTari(p2pool* pool, std::string host, const std::string& wallet)
	: m_chainParams{}
	, m_auxWallet(wallet)
	, m_pool(pool)
	, m_server(new TariServer(pool->params().m_socks5Proxy))
	, m_hostStr(host)
{
	if (host.find(TARI_PREFIX) != 0) {
		LOGERR(1, "Invalid host " << host << " - \"" << TARI_PREFIX << "\" prefix not found");
		throw std::exception();
	}

	host.erase(0, sizeof(TARI_PREFIX) - 1);

	while (host.back() == '/') {
		host.pop_back();
	}

	if (host.empty()) {
		LOGERR(1, "Invalid host");
		throw std::exception();
	}

	m_server->parse_address_list(host,
		[this](bool is_v6, const std::string& /*address*/, std::string ip, int port)
		{
			if (!m_pool->params().m_dns || resolve_host(ip, is_v6)) {
				m_server->m_TariNodeIsV6 = is_v6;
				m_server->m_TariNodeHost = ip;
				m_server->m_TariNodePort = port;
			}
		});

	if (m_server->m_TariNodeHost.empty() || (m_server->m_TariNodePort == 0) || (m_server->m_TariNodePort >= 65536)) {
		LOGERR(1, "Invalid host " << host);
		throw std::exception();
	}

	uv_rwlock_init_checked(&m_lock);

	if (!m_server->start()) {
		throw std::exception();
	}

	char buf[32] = {};
	log::Stream s(buf);
	s << "127.0.0.1:" << m_server->external_listen_port();

	m_TariNode = new BaseNode::Stub(grpc::CreateChannel(buf, grpc::InsecureChannelCredentials()));

	merge_mining_get_chain_id();
}

MergeMiningClientTari::~MergeMiningClientTari()
{
	m_server->shutdown_tcp();
	delete m_server;

	delete m_TariNode;

	LOGINFO(1, "stopped");
}

bool MergeMiningClientTari::get_params(ChainParameters& out_params) const
{
	ReadLock lock(m_lock);

	if (m_chainParams.aux_id.empty() || m_chainParams.aux_diff.empty()) {
		return false;
	}

	out_params = m_chainParams;
	return true;
}

void MergeMiningClientTari::submit_solution(const std::vector<uint8_t>& blob, const std::vector<hash>& merkle_proof)
{
	(void)blob;
	(void)merkle_proof;
}

void MergeMiningClientTari::merge_mining_get_chain_id()
{
	struct Work
	{
		uv_work_t req;
		MergeMiningClientTari* client;
	};

	Work* work = new Work{};
	work->req.data = work;
	work->client = this;

	uv_queue_work(m_server->get_loop(), &work->req,
		[](uv_work_t* req)
		{
			BACKGROUND_JOB_START(MergeMiningClientTari::merge_mining_get_chain_id);

			MergeMiningClientTari* client = reinterpret_cast<Work*>(req->data)->client;

			grpc::Status status;

			NewBlockTemplateRequest request;
			PowAlgo* algo = new PowAlgo();
			algo->set_pow_algo(PowAlgo_PowAlgos_POW_ALGOS_RANDOMX);
			request.clear_algo();
			request.set_allocated_algo(algo);
			request.set_max_weight(1);

			grpc::ClientContext ctx;
			NewBlockTemplateResponse response;
			status = client->m_TariNode->GetNewBlockTemplate(&ctx, request, &response);

			grpc::ClientContext ctx2;
			GetNewBlockResult response2;
			status = client->m_TariNode->GetNewBlock(&ctx2, response.new_block_template(), &response2);

			const std::string& id = response2.tari_unique_id();
			LOGINFO(1, client->m_hostStr << " uses chain_id " << log::LightCyan() << log::hex_buf(id.data(), id.size()));

			if (id.size() == HASH_SIZE) {
				WriteLock lock(client->m_lock);
				std::copy(id.begin(), id.end(), client->m_chainParams.aux_id.h);
			}
			else {
				LOGERR(1, "Tari unique_id has invalid size (" << id.size() << ')');
			}
		},
		[](uv_work_t* req, int /*status*/)
		{
			delete reinterpret_cast<Work*>(req->data);
			BACKGROUND_JOB_STOP(MergeMiningClientTari::merge_mining_get_chain_id);
		});
}

// TariServer and TariClient are simply a proxy from a localhost TCP port to the external Tari node
// This is needed for SOCKS5 proxy support (gRPC library doesn't support it natively)

MergeMiningClientTari::TariServer::TariServer(const std::string& socks5Proxy)
	: TCPServer(1, MergeMiningClientTari::TariClient::allocate, socks5Proxy)
	, m_TariNodeIsV6(false)
	, m_TariNodeHost()
	, m_TariNodePort(0)
	, m_internalPort(0)
{
	m_callbackBuf.resize(MergeMiningClientTari::BUF_SIZE);
}

bool MergeMiningClientTari::TariServer::start()
{
	std::random_device rd;

	for (size_t i = 0; i < 10; ++i) {
		if (start_listening(false, "127.0.0.1", 49152 + (rd() % 16384))) {
			break;
		}
	}

	if (m_listenPort < 0) {
		LOGERR(1, "failed to listen on TCP port");
		return false;
	}

	const int err = uv_thread_create(&m_loopThread, loop, this);
	if (err) {
		LOGERR(1, "failed to start event loop thread, error " << uv_err_name(err));
		return false;
	}

	m_loopThreadCreated = true;
	return true;
}

bool MergeMiningClientTari::TariServer::connect_upstream(TariClient* downstream)
{
	const bool is_v6 = m_TariNodeIsV6;
	const std::string& ip = m_TariNodeHost;
	const int port = m_TariNodePort;

	TariClient* upstream = static_cast<TariClient*>(get_client());

	upstream->m_owner = this;
	upstream->m_port = port;
	upstream->m_isV6 = is_v6;

	if (!str_to_ip(is_v6, ip.c_str(), upstream->m_addr)) {
		return_client(upstream);
		return false;
	}

	log::Stream s(upstream->m_addrString);
	if (is_v6) {
		s << '[' << ip << "]:" << port << '\0';
	}
	else {
		s << ip << ':' << port << '\0';
	}

	if (!connect_to_peer(upstream)) {
		return false;
	}

	upstream->m_pairedClient = downstream;
	upstream->m_pairedClientSavedResetCounter = downstream->m_resetCounter;

	downstream->m_pairedClient = upstream;
	downstream->m_pairedClientSavedResetCounter = upstream->m_resetCounter;

	return true;
}

void MergeMiningClientTari::TariServer::on_shutdown()
{
}

const char* MergeMiningClientTari::TariServer::get_log_category() const
{
	return log_category_prefix;
}

MergeMiningClientTari::TariClient::TariClient()
	: Client(m_buf, sizeof(m_buf))
	, m_pairedClient(nullptr)
	, m_pairedClientSavedResetCounter(std::numeric_limits<uint32_t>::max())
{
	m_buf[0] = '\0';
}

void MergeMiningClientTari::TariClient::reset()
{
	if (is_paired()) {
		m_pairedClient->m_pairedClient = nullptr;
		m_pairedClient->close();
		m_pairedClient = nullptr;
	}
	m_pairedClientSavedResetCounter = std::numeric_limits<uint32_t>::max();
}

bool MergeMiningClientTari::TariClient::on_connect()
{
	MergeMiningClientTari::TariServer* server = static_cast<MergeMiningClientTari::TariServer*>(m_owner);
	if (!server) {
		return false;
	}

	if (m_isIncoming) {
		return server->connect_upstream(this);
	}

	return true;
}

bool MergeMiningClientTari::TariClient::on_read(char* data, uint32_t size)
{
	MergeMiningClientTari::TariServer* server = static_cast<MergeMiningClientTari::TariServer*>(m_owner);
	if (!server) {
		return false;
	}

	if (!is_paired()) {
		return false;
	}

	return server->send(m_pairedClient,
		[data, size](uint8_t* buf, size_t buf_size) -> size_t
		{
			if (size > buf_size) {
				return 0U;
			}

			memcpy(buf, data, size);
			return size;
		});
}

} // namespace p2pool
