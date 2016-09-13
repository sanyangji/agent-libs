#pragma once

#include <cstdint>

// A simple token bucket that accumulates tokens at a fixed rate and allows
// for limited bursting in the form of "banked" tokens.
class token_bucket
{
public:
	token_bucket();
	virtual ~token_bucket();

	//
	// Initialize the token bucket and start accumulating tokens
	//
	void init(uint32_t rate, uint32_t max_tokens);

	//
	// Returns true if a token can be claimed. Also updates
	// internal metrics.
	//
	bool claim();
private:

	// Utility function to get the time in nanoseconds since the epoch.
	uint64_t get_epoch_ns();

	//
	// The number of tokens generated per second.
	//
	uint64_t m_rate;

	//
	// The maximum number of tokens that can be banked for future
	// claim()s.
	//
	uint64_t m_max_tokens;

	//
	// The current number of tokens
	//
	uint64_t m_tokens;

	//
	// The last time claim() was called (or the object was created).
	// Nanoseconds since the epoch.
	//
	uint64_t m_last_seen;
};

