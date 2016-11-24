﻿#include "NonceSubmitter.hpp"
#include "MinerLogger.h"
#include "Deadline.hpp"
#include "MinerUtil.h"
#include "nxt/nxt_address.h"
#include "Request.hpp"
#include "MinerConfig.h"
#include "Miner.h"

Burst::NonceSubmitter::NonceSubmitter(Miner& miner, std::shared_ptr<Deadline> deadline)
	: miner_(&miner), deadline_(deadline)
{}

void Burst::NonceSubmitter::startSubmit()
{
	std::thread t(&NonceSubmitter::submitThread, this, miner_, deadline_);
    t.detach();
}

void Burst::NonceSubmitter::submitThread(Miner* miner, std::shared_ptr<Deadline> deadline) const
{
	static uint32_t submitThreads = 0;

	++submitThreads;

	MinerLogger::write(std::to_string(submitThreads) + " submitter-threads running", TextType::Debug);

	auto nonce = deadline->getNonce();
	auto deadlineValue = deadline->getDeadline();
	auto accountId = deadline->getAccountId();

	auto loopConditionHelper = [deadline, miner](size_t tryCount, size_t maxTryCount, SubmitResponse response)
	{
		if (tryCount >= maxTryCount ||
			response == SubmitResponse::Submitted ||
			deadline->getBlock() != miner->getBlockheight())
			return false;

		auto bestSent = miner->getBestSent(deadline->getAccountId(), deadline->getBlock());
		auto betterDeadlineInPipeline = false;

		if (bestSent != nullptr)
		{
			betterDeadlineInPipeline = bestSent->getDeadline() < deadline->getDeadline();
			MinerLogger::write("Best sent nonce so far: " + bestSent->deadlineToReadableString() + " vs. this deadline: "
				+ deadlineFormat(deadline->getDeadline()), TextType::Debug);
		}

		if (betterDeadlineInPipeline)
		{
			MinerLogger::write("Better deadline in pipeline (" + deadlineFormat(bestSent->getDeadline()) +
				"), stop send this one (" + deadlineFormat(deadline->getDeadline()) + ")", TextType::Debug);
			return false;
		}

		return true;
	};

	//MinerLogger::write("sending nonce from thread, " + deadlineFormat(deadlineValue), TextType::System);

	NxtAddress addr(accountId);
	MinerLogger::write(addr.to_string() + ": nonce on the way (" + deadline->deadlineToReadableString() + ")");

	NonceRequest request(miner->getSocket());
	NonceConfirmation confirmation { 0, SubmitResponse::None };
	size_t submitTryCount = 0;
	auto firstSendAttempt = true;

	// submit-loop
	while (loopConditionHelper(submitTryCount,
							   MinerConfig::getConfig().getSubmissionMaxRetry(),
							   confirmation.errorCode))
	{
		auto sendTryCount = 0u;

		MinerLogger::write("Submit-loop " + std::to_string(submitTryCount + 1) + " (" + deadline->deadlineToReadableString() + ") ["
						   + std::to_string(reinterpret_cast<uintptr_t>(deadline.get())) + "]", TextType::Debug);

		// send-loop
		while (loopConditionHelper(sendTryCount,
								   MinerConfig::getConfig().getSendMaxRetry(),
								   confirmation.errorCode))
		{
			auto response = request.submit(nonce, accountId, deadlineValue);
			auto receiveTryCount = 0u;

			if (response.canReceive() && firstSendAttempt)
			{
				MinerLogger::write(addr.to_string() + ": nonce submitted (" + deadlineFormat(deadlineValue) + ")", TextType::Ok);
				firstSendAttempt = false;
			}

			MinerLogger::write("Send-loop " + std::to_string(sendTryCount + 1) + " (" + deadline->deadlineToReadableString() + ") ["
							   + std::to_string(reinterpret_cast<uintptr_t>(deadline.get())) + "]", TextType::Debug);

			// receive-loop
			while (loopConditionHelper(receiveTryCount,
									   MinerConfig::getConfig().getReceiveMaxRetry(),
									   confirmation.errorCode))
			{
				MinerLogger::write("Receive-loop " + std::to_string(receiveTryCount + 1) + " (" + deadline->deadlineToReadableString() + ") ["
								   + std::to_string(reinterpret_cast<uintptr_t>(deadline.get())) + "]", TextType::Debug);
				confirmation = response.getConfirmation();
				++receiveTryCount;
			}

			transferSocket(response, request);

			++sendTryCount;
		}

		++submitTryCount;
	}

	if (confirmation.errorCode == SubmitResponse::Submitted)
	{
		deadline->confirm();
		MinerLogger::write(NxtAddress(accountId).to_string() + ": nonce confirmed (" + deadlineFormat(deadlineValue) + ")", TextType::Success);
	}
	// sent, but not confirmed
	else if (firstSendAttempt &&
		confirmation.errorCode == SubmitResponse::None)
	{
		MinerLogger::write(NxtAddress(accountId).to_string() + ": got no confirmation from server! busy?", TextType::Normal);
	}

	--submitThreads;

	MinerLogger::write(std::to_string(submitThreads) + " submitter-threads running", TextType::Debug);
}
