#include <cstring>

#include <iostream>
#include <thread>
#include <stdexcept>
#include <source_location>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <string>
#include <atomic>
#include <barrier>

extern "C" {
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_errno.h>
}


namespace {

 fi_info* makeHints() {

  fi_info *hints = fi_allocinfo();
  if (!hints) {
    throw std::runtime_error("Hints allocation failed");
  }

  hints->domain_attr->threading = FI_THREAD_SAFE;
  hints->domain_attr->mr_mode =
      FI_MR_LOCAL | FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_VIRT_ADDR;
  hints->domain_attr->name = nullptr;
  hints->fabric_attr->prov_name = strdup("shm");

  hints->domain_attr->resource_mgmt = FI_RM_ENABLED;
  hints->ep_attr->type = FI_EP_RDM;
  hints->ep_attr->protocol = FI_PROTO_UNSPEC;
  hints->addr_format = FI_FORMAT_UNSPEC;
  hints->dest_addr = nullptr;
  hints->domain_attr->control_progress = FI_PROGRESS_MANUAL;
  hints->domain_attr->data_progress = FI_PROGRESS_MANUAL;
  hints->caps = FI_MSG |  FI_RMA | FI_TAGGED | FI_SOURCE |  FI_DIRECTED_RECV;
  hints->tx_attr->op_flags = FI_DELIVERY_COMPLETE;

  return hints;
}

fi_cq_attr initCqAttrDefault() {
  fi_cq_attr attr{};
  // VERY important setting - without that the sync data sent with fi_(...)data
  // instructions (like fi_writedata) is not formatted properly and cannot be
  // read properly
  attr.format = FI_CQ_FORMAT_TAGGED;
  // These 2 settings are needed to use fi_cq_sread
  attr.wait_obj = FI_WAIT_UNSPEC;
  attr.wait_cond = FI_CQ_COND_NONE;
  return attr;
}

fi_av_attr initAvAttrDefault() {
  fi_av_attr avAttr{};
  std::memset(&avAttr, 0, sizeof(fi_av_attr));
  avAttr.type = FI_AV_MAP;
  return avAttr;
}

auto getAddr(fid *endpoint) {
  std::size_t addrLen = 0;
  auto ret = fi_getname(endpoint, nullptr, &addrLen);
  if ((ret != -FI_ETOOSMALL) || (addrLen <= 0)) {
    throw std::runtime_error("Unexpected error");
  }

  auto data = std::vector<char>(addrLen, 0);

  ret = fi_getname(endpoint, static_cast<void *>(data.data()), &addrLen);
  if (ret) {
    throw std::runtime_error("Unexpected error");
  }

  data.shrink_to_fit();
  return data;
}

void CHECK(int ret, const std::source_location location = std::source_location::current()) {
  if (ret) {
    std::ostringstream oss;
    oss << "Check failed file: " << location.file_name()
        << "(" << location.line() << ":" << location.column() << ") `"
        << location.function_name() << "`: " << fi_strerror(ret);

    throw std::runtime_error(oss.str());
  }
}

}

int main() {

setenv("FI_SHM_DISABLE_CMA", "1", 0 /*overwrite*/);

static constexpr size_t bufferSize{1000};
static constexpr size_t numClients{4};

std::vector<char> serverAddr{};
std::vector<std::vector<char>> clientAddr(numClients, std::vector<char>{});
std::atomic<size_t> clientReadyCnt = 0;
uint32_t serverAddrFormat = 0;
std::mutex serverDataMtx;
std::condition_variable serverCv;
std::mutex clientDataMtx;
std::condition_variable clientCv;

bool serverDataReady = false;
bool clientDataReady = false;

// std::barrier sync_point(numClients+1);

std::thread server([&]{
    fi_info *hints = makeHints();
    fi_info *info = nullptr;
    fid_fabric *fabric = nullptr;
    fid_domain *domain = nullptr;
    fid_ep *ep = nullptr;
    fid_cq *rxCq = nullptr;
    fid_cq *txCq = nullptr;
    fid_av *av = nullptr;

    auto rxCqAttr = initCqAttrDefault();
    auto txCqAtrr = initCqAttrDefault();
    auto avAttr = initAvAttrDefault();

    CHECK(fi_getinfo(fi_version(), nullptr, nullptr, 0,
                            hints, &info));
    CHECK(fi_fabric(info->fabric_attr, &fabric, nullptr));
    CHECK(fi_domain(fabric, info, &domain, nullptr));
    CHECK(fi_endpoint(domain, info, &ep, nullptr));
    CHECK(fi_cq_open(domain, &rxCqAttr, &rxCq, nullptr));
    CHECK(fi_cq_open(domain, &txCqAtrr, &txCq, nullptr));
    CHECK(fi_av_open(domain, &avAttr, &av, nullptr));

    CHECK(fi_ep_bind(ep, &txCq->fid, FI_TRANSMIT));
    CHECK(fi_ep_bind(ep, &rxCq->fid, FI_RECV));
    CHECK(fi_ep_bind(ep, &av->fid, 0));
    CHECK(fi_enable(ep));

    char** buf =  (char**) malloc(numClients * sizeof(char*));
    for (int i = 0; i < numClients; i++) {
      buf[i] = (char*) malloc(bufferSize);
    }

    std::size_t PP_MR_KEY = 0xC0DE;
    std::vector<fid_mr *>mrs(numClients, nullptr);
    for (size_t i = 0; i < numClients; ++i)
    {
        auto flags = FI_SEND | FI_RECV | FI_REMOTE_READ | FI_REMOTE_WRITE;
        fi_mr_reg(domain, buf[i], bufferSize, flags, 0, PP_MR_KEY + i, 0, &mrs[i], nullptr);
    }

    const auto localAddr = getAddr(&ep->fid);
    fi_addr_t localFiAddr = 0;

    if (info->domain_attr->caps & FI_LOCAL_COMM) {
        CHECK(fi_av_insert(av, localAddr.data(), 1, &localFiAddr, 0, nullptr) < 0);
    }

    {
        std::unique_lock lock(serverDataMtx);
        serverAddr = localAddr;
        serverAddrFormat = info->addr_format;
        serverDataReady = true;
        serverCv.notify_all();
    }

    std::unique_lock lock(clientDataMtx);
    clientCv.wait(lock, [&] { return clientReadyCnt == numClients; });

    std::vector<fi_addr_t> remoteFiAddrs(numClients, 0);
        for (auto i = 0U; i < numClients; ++i)
      CHECK(fi_av_insert(av, clientAddr[i].data(), 1, &remoteFiAddrs[i], 0, nullptr) < 0);


    for (auto i = 0U; i < numClients; ++i)
      while (fi_recv(ep, buf[i], bufferSize, fi_mr_desc(mrs[i]), remoteFiAddrs[i], nullptr) != 0);

    fi_cq_err_entry comp{};

    while (fi_cq_readfrom(rxCq, &comp, 1, &comp.src_addr) < 0);

    // sync_point.arrive_and_wait();

    for (auto & mr : mrs)
      fi_close(&mr->fid);

    for (int i = 0; i < numClients; i++)
      free(buf[i]);

    free(buf);

    fi_close(&ep->fid);
    fi_close(&av->fid);
    fi_close(&rxCq->fid);
    fi_close(&txCq->fid);
    fi_close(&domain->fid);
    fi_close(&fabric->fid);
    fi_freeinfo(info);
    fi_freeinfo(hints);
});

std::vector<std::thread> clients;
clients.reserve(numClients);

for (auto i = 0U; i < numClients; i++) {
  clients.emplace_back([&, id = i]{
    fi_info *hints = makeHints();
    fi_info *info = nullptr;
    fid_fabric *fabric = nullptr;
    fid_domain *domain = nullptr;
    fid_ep *ep = nullptr;
    fid_cq *rxCq = nullptr;
    fid_cq *txCq = nullptr;
    fid_av *av = nullptr;
    auto rxCqAttr = initCqAttrDefault();
    auto txCqAtrr = initCqAttrDefault();
    auto avAttr = initAvAttrDefault();

    {
      std::unique_lock lock(serverDataMtx);
      serverCv.wait(lock, [&] { return serverDataReady; });
    }

    hints->dest_addrlen = serverAddr.size();
    hints->addr_format = serverAddrFormat;
    hints->dest_addr = serverAddr.data();

    CHECK(fi_getinfo(fi_version(), nullptr, nullptr, 0,
                            hints, &info));

    CHECK(fi_fabric(info->fabric_attr, &fabric, nullptr));
    CHECK(fi_domain(fabric, info, &domain, nullptr));
    CHECK(fi_endpoint(domain, info, &ep, nullptr));
    CHECK(fi_cq_open(domain, &rxCqAttr, &rxCq, nullptr));
    CHECK(fi_cq_open(domain, &txCqAtrr, &txCq, nullptr));
    CHECK(fi_av_open(domain, &avAttr, &av, nullptr));

    CHECK(fi_ep_bind(ep, &txCq->fid, FI_TRANSMIT));
    CHECK(fi_ep_bind(ep, &rxCq->fid, FI_RECV));
    CHECK(fi_ep_bind(ep, &av->fid, 0));
    CHECK(fi_enable(ep));

    auto buf = malloc(bufferSize);
    std::size_t PP_MR_KEY = 0xC0DE;
    fid_mr *mr = nullptr;
    auto flags = FI_SEND | FI_RECV;
    CHECK(fi_mr_reg(domain, buf, bufferSize, flags, 0, PP_MR_KEY, 0, &(mr), nullptr));

    const auto localAddr = getAddr(&ep->fid);
    {
        std::unique_lock lock(clientDataMtx);
        clientAddr[id] = localAddr;
        ++clientReadyCnt;
        clientCv.notify_all();
    }

    fi_addr_t localFiAddr = 0;
    if (info->domain_attr->caps & FI_LOCAL_COMM) {
        auto ret = fi_av_insert(av, localAddr.data(), 1, &localFiAddr, 0, nullptr);
        CHECK(ret < 0);
        CHECK(ret != 1);
    }

    fi_addr_t remoteFiAddr = 0;
    CHECK(fi_av_insert(av, hints->dest_addr, 1, &remoteFiAddr, 0, nullptr) < 0);

    while (fi_send(ep, buf, bufferSize, fi_mr_desc(mr), remoteFiAddr, nullptr) != 0);

    fi_cq_err_entry comp{};

    while (fi_cq_readfrom(txCq, &comp, 1, &comp.src_addr) < 0);

    // sync_point.arrive_and_wait();

    fi_close(&mr->fid);
    fi_close(&ep->fid);
    free(buf);
    fi_close(&av->fid);
    fi_close(&rxCq->fid);
    fi_close(&txCq->fid);
    fi_close(&domain->fid);
    fi_close(&fabric->fid);
    fi_freeinfo(info);
    hints->dest_addr = nullptr;
    fi_freeinfo(hints);
});
}

server.join();
for (auto & client : clients)
  client.join();

}

