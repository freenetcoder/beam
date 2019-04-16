// Copyright 2019 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "bitcoin_side.h"

#include "bitcoin/bitcoin.hpp"
#include "nlohmann/json.hpp"

#include "common.h"

using namespace ECC;
using json = nlohmann::json;

namespace
{
    uint32_t kBTCLockTimeSec = 2 * 24 * 60 * 60;
    uint32_t kBTCMinTxConfirmations = 6;

    libbitcoin::chain::script AtomicSwapContract(const libbitcoin::short_hash& hashPublicKeyA
        , const libbitcoin::short_hash& hashPublicKeyB
        , int64_t locktime
        , const libbitcoin::data_chunk& secretHash
        , size_t secretSize)
    {
        using namespace libbitcoin::machine;

        operation::list contract_operations;

        contract_operations.emplace_back(operation(opcode::if_)); // Normal redeem path
        {
            // Require initiator's secret to be a known length that the redeeming
            // party can audit.  This is used to prevent fraud attacks between two
            // currencies that have different maximum data sizes.
            contract_operations.emplace_back(operation(opcode::size));
            operation secretSizeOp;
            secretSizeOp.from_string(std::to_string(secretSize));
            contract_operations.emplace_back(secretSizeOp);
            contract_operations.emplace_back(operation(opcode::equalverify));

            // Require initiator's secret to be known to redeem the output.
            contract_operations.emplace_back(operation(opcode::sha256));
            contract_operations.emplace_back(operation(secretHash));
            contract_operations.emplace_back(operation(opcode::equalverify));

            // Verify their signature is being used to redeem the output.  This
            // would normally end with OP_EQUALVERIFY OP_CHECKSIG but this has been
            // moved outside of the branch to save a couple bytes.
            contract_operations.emplace_back(operation(opcode::dup));
            contract_operations.emplace_back(operation(opcode::hash160));
            contract_operations.emplace_back(operation(libbitcoin::to_chunk(hashPublicKeyB)));
        }
        contract_operations.emplace_back(operation(opcode::else_)); // Refund path
        {
            // Verify locktime and drop it off the stack (which is not done by CLTV).
            operation locktimeOp;
            locktimeOp.from_string(std::to_string(locktime));
            contract_operations.emplace_back(locktimeOp);
            contract_operations.emplace_back(operation(opcode::checklocktimeverify));
            contract_operations.emplace_back(operation(opcode::drop));

            // Verify our signature is being used to redeem the output.  This would
            // normally end with OP_EQUALVERIFY OP_CHECKSIG but this has been moved
            // outside of the branch to save a couple bytes.
            contract_operations.emplace_back(operation(opcode::dup));
            contract_operations.emplace_back(operation(opcode::hash160));
            contract_operations.emplace_back(operation(libbitcoin::to_chunk(hashPublicKeyA)));
        }
        contract_operations.emplace_back(operation(opcode::endif));

        // Complete the signature check.
        contract_operations.emplace_back(operation(opcode::equalverify));
        contract_operations.emplace_back(operation(opcode::checksig));

        return libbitcoin::chain::script(contract_operations);
    }
}

namespace beam::wallet
{
    BitcoinSide::BitcoinSide(BaseTransaction& tx, std::shared_ptr<BitcoinRPC> bitcoinRPC, bool isInitiator, bool isBtcOwner)
        : m_tx(tx)
        , m_bitcoinRPC(bitcoinRPC)
        , m_isInitiator(isInitiator)
        , m_isBtcOwner(isBtcOwner)
    {
    }

    bool BitcoinSide::Initial()
    {
        if (!LoadSwapAddress())
            return false;

        if (m_isBtcOwner)
        {
            InitSecret();
        }

        return true;
    }

    void BitcoinSide::InitLockTime()
    {
        auto externalLockTime = m_tx.GetMandatoryParameter<Timestamp>(TxParameterID::CreateTime) + kBTCLockTimeSec;
        m_tx.SetParameter(TxParameterID::AtomicSwapExternalLockTime, externalLockTime);
    }

    void BitcoinSide::AddTxDetails(SetTxParameter& txParameters)
    {
        auto txID = m_tx.GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapExternalTxID, SubTxIndex::LOCK_TX);
        uint32_t outputIndex = m_tx.GetMandatoryParameter<uint32_t>(TxParameterID::AtomicSwapExternalTxOutputIndex, SubTxIndex::LOCK_TX);
        std::string swapAddress = m_tx.GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapAddress);

        txParameters.AddParameter(TxParameterID::AtomicSwapPeerAddress, swapAddress)
            .AddParameter(TxParameterID::SubTxIndex, SubTxIndex::LOCK_TX)
            .AddParameter(TxParameterID::AtomicSwapExternalTxID, txID)
            .AddParameter(TxParameterID::AtomicSwapExternalTxOutputIndex, outputIndex);
    }

    bool BitcoinSide::ConfirmLockTx()
    {
        // wait TxID from peer
        std::string txID;
        if (!m_tx.GetParameter(TxParameterID::AtomicSwapExternalTxID, txID, SubTxIndex::LOCK_TX))
            return false;

        if (m_SwapLockTxConfirmations < kBTCMinTxConfirmations)
        {
            // validate expired?

            GetSwapLockTxConfirmations();
            return false;
        }

        return true;
    }

    bool BitcoinSide::SendLockTx()
    {
        auto lockTxState = BuildLockTx();
        if (lockTxState != SwapTxState::Constructed)
            return false;

        // send contractTx
        assert(m_SwapLockRawTx.is_initialized());

        if (!RegisterTx(*m_SwapLockRawTx, SubTxIndex::LOCK_TX))
            return false;

        return true;
    }

    bool BitcoinSide::SendRefund()
    {
        return SendWithdrawTx(SubTxIndex::REFUND_TX);
    }

    bool BitcoinSide::SendRedeem()
    {
        return SendWithdrawTx(SubTxIndex::REDEEM_TX);
    }

    bool BitcoinSide::LoadSwapAddress()
    {
        // load or generate BTC address
        if (std::string swapAddress; !m_tx.GetParameter(TxParameterID::AtomicSwapAddress, swapAddress))
        {
            // is need to setup type 'legacy'?
            m_bitcoinRPC->getRawChangeAddress(BIND_THIS_MEMFN(OnGetRawChangeAddress));

            return false;
        }
        return true;
    }

    void BitcoinSide::InitSecret()
    {
        NoLeak<uintBig> preimage;
        GenRandom(preimage.V);
        m_tx.SetParameter(TxParameterID::PreImage, preimage.V, false, BEAM_REDEEM_TX);
    }

    libbitcoin::chain::script BitcoinSide::CreateAtomicSwapContract()
    {
        Timestamp locktime = m_tx.GetMandatoryParameter<Timestamp>(TxParameterID::AtomicSwapExternalLockTime);
        std::string peerSwapAddress = m_tx.GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapPeerAddress);
        std::string swapAddress = m_tx.GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapAddress);

        // load secret or secretHash
        Hash::Value lockImage(Zero);

        if (NoLeak<uintBig> preimage; m_tx.GetParameter(TxParameterID::PreImage, preimage.V, SubTxIndex::BEAM_REDEEM_TX))
        {
            Hash::Processor() << preimage.V >> lockImage;
        }
        else
        {
            lockImage = m_tx.GetMandatoryParameter<uintBig>(TxParameterID::PeerLockImage, SubTxIndex::BEAM_REDEEM_TX);
        }

        libbitcoin::data_chunk secretHash = libbitcoin::to_chunk(lockImage.m_pData);
        libbitcoin::wallet::payment_address senderAddress(m_isBtcOwner ? swapAddress : peerSwapAddress);
        libbitcoin::wallet::payment_address receiverAddress(m_isBtcOwner ? peerSwapAddress : swapAddress);

        return AtomicSwapContract(senderAddress.hash(), receiverAddress.hash(), locktime, secretHash, secretHash.size());
    }

    bool BitcoinSide::RegisterTx(const std::string& rawTransaction, SubTxID subTxID)
    {
        bool isRegistered = false;
        if (!m_tx.GetParameter(TxParameterID::TransactionRegistered, isRegistered, subTxID))
        {
            auto callback = [this, subTxID](const std::string& response) {
                json reply = json::parse(response);
                assert(reply["error"].empty());

                auto txID = reply["result"].get<std::string>();
                bool isRegistered = !txID.empty();
                m_tx.SetParameter(TxParameterID::TransactionRegistered, isRegistered, false, subTxID);

                if (!txID.empty())
                {
                    m_tx.SetParameter(TxParameterID::AtomicSwapExternalTxID, txID, false, subTxID);
                }

                m_tx.Update();
            };

            m_bitcoinRPC->sendRawTransaction(rawTransaction, callback);
            return isRegistered;
        }

        if (!isRegistered)
        {
            // TODO roman.strilec 
            //m_tx.OnFailed(TxFailureReason::FailedToRegister, true);
        }

        return isRegistered;
    }

    SwapTxState BitcoinSide::BuildLockTx()
    {
        SwapTxState swapTxState = SwapTxState::Initial;
        m_tx.GetParameter(TxParameterID::State, swapTxState, SubTxIndex::LOCK_TX);

        if (swapTxState == SwapTxState::Initial)
        {
            auto contractScript = CreateAtomicSwapContract();
            Amount swapAmount = m_tx.GetMandatoryParameter<Amount>(TxParameterID::AtomicSwapAmount);

            libbitcoin::chain::transaction contractTx;
            libbitcoin::chain::output output(swapAmount, contractScript);
            contractTx.outputs().push_back(output);

            std::string hexTx = libbitcoin::encode_base16(contractTx.to_data());

            m_bitcoinRPC->fundRawTransaction(hexTx, BIND_THIS_MEMFN(OnFundRawTransaction));

            m_tx.SetState(SwapTxState::CreatingTx, SubTxIndex::LOCK_TX);
            return SwapTxState::CreatingTx;
        }

        if (swapTxState == SwapTxState::CreatingTx)
        {
            // TODO: implement
        }

        // TODO: check
        return swapTxState;
    }

    SwapTxState BitcoinSide::BuildWithdrawTx(SubTxID subTxID)
    {
        SwapTxState swapTxState = SwapTxState::Initial;
        m_tx.GetParameter(TxParameterID::State, swapTxState, subTxID);

        if (swapTxState == SwapTxState::Initial)
        {
            // TODO: implement fee calculation
            Amount fee = 1000;

            Amount swapAmount = m_tx.GetMandatoryParameter<Amount>(TxParameterID::AtomicSwapAmount);
            swapAmount = swapAmount - fee;
            std::string swapAddress = m_tx.GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapAddress);
            uint32_t outputIndex = m_tx.GetMandatoryParameter<uint32_t>(TxParameterID::AtomicSwapExternalTxOutputIndex, SubTxIndex::LOCK_TX);
            auto swapLockTxID = m_tx.GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapExternalTxID, SubTxIndex::LOCK_TX);

            std::vector<std::string> args;
            args.emplace_back("[{\"txid\": \"" + swapLockTxID + "\", \"vout\":" + std::to_string(outputIndex) + ", \"Sequence\": " + std::to_string(libbitcoin::max_input_sequence - 1) + " }]");
            args.emplace_back("[{\"" + swapAddress + "\": " + std::to_string(double(swapAmount) / libbitcoin::satoshi_per_bitcoin) + "}]");
            if (subTxID == SubTxIndex::REFUND_TX)
            {
                Timestamp locktime = m_tx.GetMandatoryParameter<Timestamp>(TxParameterID::AtomicSwapExternalLockTime);
                args.emplace_back(std::to_string(locktime));
            }

            m_bitcoinRPC->createRawTransaction(args, BIND_THIS_MEMFN(OnCreateWithdrawTransaction));
            m_tx.SetState(SwapTxState::CreatingTx, subTxID);
            return SwapTxState::CreatingTx;
        }

        if (swapTxState == SwapTxState::CreatingTx)
        {
            std::string swapAddress = m_tx.GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapAddress);
            auto callback = [this, subTxID](const std::string& response) {
                OnDumpPrivateKey(subTxID, response);
            };

            m_bitcoinRPC->dumpPrivKey(swapAddress, callback);
        }

        if (swapTxState == SwapTxState::Constructed && !m_SwapWithdrawRawTx.is_initialized())
        {
            m_SwapWithdrawRawTx = m_tx.GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapExternalTx, subTxID);
        }

        return swapTxState;
    }

    void BitcoinSide::GetSwapLockTxConfirmations()
    {
        auto txID = m_tx.GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapExternalTxID, SubTxIndex::LOCK_TX);
        uint32_t outputIndex = m_tx.GetMandatoryParameter<uint32_t>(TxParameterID::AtomicSwapExternalTxOutputIndex, SubTxIndex::LOCK_TX);

        m_bitcoinRPC->getTxOut(txID, outputIndex, BIND_THIS_MEMFN(OnGetSwapLockTxConfirmations));
    }

    bool BitcoinSide::SendWithdrawTx(SubTxID subTxID)
    {
        if (bool isRegistered = false; !m_tx.GetParameter(TxParameterID::TransactionRegistered, isRegistered, subTxID))
        {
            auto refundTxState = BuildWithdrawTx(subTxID);
            if (refundTxState != SwapTxState::Constructed)
                return false;

            assert(m_SwapWithdrawRawTx.is_initialized());
        }

        if (!RegisterTx(*m_SwapWithdrawRawTx, subTxID))
            return false;

        // TODO: check confirmations

        return true;
    }

    void BitcoinSide::OnGetRawChangeAddress(const std::string& response)
    {
        json reply = json::parse(response);
        assert(reply["error"].empty());

        // TODO: validate error
        // const auto& error = reply["error"];

        const auto& result = reply["result"];

        // Don't need overwrite existing address
        if (std::string swapAddress; !m_tx.GetParameter(TxParameterID::AtomicSwapAddress, swapAddress))
        {
            m_tx.SetParameter(TxParameterID::AtomicSwapAddress, result.get<std::string>());
        }

        m_tx.UpdateAsync();
    }

    void BitcoinSide::OnFundRawTransaction(const std::string& response)
    {
        json reply = json::parse(response);
        assert(reply["error"].empty());

        //const auto& error = reply["error"];
        const auto& result = reply["result"];
        auto hexTx = result["hex"].get<std::string>();
        int changePos = result["changepos"].get<int>();

        // float fee = result["fee"].get<float>();      // calculate fee!
        uint32_t valuePosition = changePos ? 0 : 1;
        m_tx.SetParameter(TxParameterID::AtomicSwapExternalTxOutputIndex, valuePosition, false, SubTxIndex::LOCK_TX);

        m_bitcoinRPC->signRawTransaction(hexTx, BIND_THIS_MEMFN(OnSignLockTransaction));
    }

    void BitcoinSide::OnSignLockTransaction(const std::string& response)
    {
        json reply = json::parse(response);
        assert(reply["error"].empty());

        const auto& result = reply["result"];

        assert(result["complete"].get<bool>());
        m_SwapLockRawTx = result["hex"].get<std::string>();

        m_tx.SetState(SwapTxState::Constructed, SubTxIndex::LOCK_TX);
        m_tx.UpdateAsync();
    }

    void BitcoinSide::OnCreateWithdrawTransaction(const std::string& response)
    {
        json reply = json::parse(response);
        assert(reply["error"].empty());
        if (!m_SwapWithdrawRawTx.is_initialized())
        {
            m_SwapWithdrawRawTx = reply["result"].get<std::string>();
            m_tx.UpdateAsync();
        }
    }

    void BitcoinSide::OnDumpPrivateKey(SubTxID subTxID, const std::string& response)
    {
        json reply = json::parse(response);
        assert(reply["error"].empty());

        const auto& result = reply["result"];

        libbitcoin::data_chunk tx_data;
        libbitcoin::decode_base16(tx_data, *m_SwapWithdrawRawTx);
        libbitcoin::chain::transaction withdrawTX = libbitcoin::chain::transaction::factory_from_data(tx_data);

        libbitcoin::wallet::ec_private wallet_key(result.get<std::string>(), libbitcoin::wallet::ec_private::testnet_wif);
        libbitcoin::endorsement sig;

        uint32_t input_index = 0;
        auto contractScript = CreateAtomicSwapContract();
        libbitcoin::chain::script::create_endorsement(sig, wallet_key.secret(), contractScript, withdrawTX, input_index, libbitcoin::machine::sighash_algorithm::all);

        // Create input script
        libbitcoin::machine::operation::list sig_script;
        libbitcoin::ec_compressed pubkey = wallet_key.to_public().point();

        if (SubTxIndex::REFUND_TX == subTxID)
        {
            // <my sig> <my pubkey> 0
            sig_script.push_back(libbitcoin::machine::operation(sig));
            sig_script.push_back(libbitcoin::machine::operation(libbitcoin::to_chunk(pubkey)));
            sig_script.push_back(libbitcoin::machine::operation(libbitcoin::machine::opcode(0)));
        }
        else
        {
            auto secret = m_tx.GetMandatoryParameter<ECC::uintBig>(TxParameterID::PreImage, SubTxIndex::BEAM_REDEEM_TX);

            // <their sig> <their pubkey> <initiator secret> 1
            sig_script.push_back(libbitcoin::machine::operation(sig));
            sig_script.push_back(libbitcoin::machine::operation(libbitcoin::to_chunk(pubkey)));
            sig_script.push_back(libbitcoin::machine::operation(libbitcoin::to_chunk(secret.m_pData)));
            sig_script.push_back(libbitcoin::machine::operation(libbitcoin::machine::opcode::push_positive_1));
        }

        libbitcoin::chain::script input_script(sig_script);

        // Add input script to first input in transaction
        withdrawTX.inputs()[0].set_script(input_script);

        // update m_SwapWithdrawRawTx
        m_SwapWithdrawRawTx = libbitcoin::encode_base16(withdrawTX.to_data());

        m_tx.SetParameter(TxParameterID::AtomicSwapExternalTx, *m_SwapWithdrawRawTx, subTxID);
        m_tx.SetState(SwapTxState::Constructed, subTxID);
        m_tx.UpdateAsync();
    }

    void BitcoinSide::OnGetSwapLockTxConfirmations(const std::string& response)
    {
        json reply = json::parse(response);
        assert(reply["error"].empty());

        const auto& result = reply["result"];

        if (result.empty())
        {
            return;
        }

        // validate amount
        {
            Amount swapAmount = m_tx.GetMandatoryParameter<Amount>(TxParameterID::AtomicSwapAmount);
            Amount outputAmount = static_cast<Amount>(std::round(result["value"].get<double>() * libbitcoin::satoshi_per_bitcoin));
            if (swapAmount > outputAmount)
            {
                LOG_DEBUG() << m_tx.GetTxID() << "Unexpected amount, excpected: " << swapAmount << ", got: " << outputAmount;

                // TODO: implement error handling
                return;
            }
        }

        // validate contract script
        libbitcoin::data_chunk scriptData;
        libbitcoin::decode_base16(scriptData, result["scriptPubKey"]["hex"]);
        auto script = libbitcoin::chain::script::factory_from_data(scriptData, false);

        auto contractScript = CreateAtomicSwapContract();

        assert(script == contractScript);

        if (script != contractScript)
        {
            // TODO: implement
            return;
        }

        // get confirmations
        m_SwapLockTxConfirmations = result["confirmations"];
    }
}