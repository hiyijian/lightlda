#include "alias_table.h"

#include "common.h"
#include "model.h"
#include "util.h"
#include "meta.h"

#include <multiverso/lock.h>
#include <multiverso/log.h>
#include <multiverso/row.h>
#include <multiverso/row_iter.h>

namespace multiverso { namespace lightlda
{
    _THREAD_LOCAL std::vector<float>* AliasTable::q_w_proportion_;
    _THREAD_LOCAL std::vector<int32_t>* AliasMultinomialRNGInt::q_proportion_int_;
    _THREAD_LOCAL std::vector<std::pair<int32_t, int32_t>>* AliasMultinomialRNGInt::L_;
    _THREAD_LOCAL std::vector<std::pair<int32_t, int32_t>>* AliasMultinomialRNGInt::H_;

    // -- AliasTable implement area --------------------------------- //
    AliasTable::AliasTable()
    {
        memory_size_ = Config::alias_capacity / sizeof(int32_t);
        num_vocabs_ = Config::num_vocabs;
        num_topics_ = Config::num_topics;
        beta_ = Config::beta;
        beta_sum_ = beta_ * num_vocabs_;
        memory_block_ = new int32_t[memory_size_];

        alias_rng_int_ = new AliasMultinomialRNGInt(num_topics_);
        
        beta_kv_vector_ = new int32_t[2 * num_topics_];

        height_.resize(num_vocabs_);
        mass_.resize(num_vocabs_);
    }

    AliasTable::~AliasTable()
    {
        delete alias_rng_int_;
        delete[] memory_block_;
        delete[] beta_kv_vector_;
    }

    void AliasTable::Init(AliasTableIndex* table_index)
    {
        table_index_ = table_index;
    }

    int32_t AliasTable::Build(int32_t word, ModelBase* model)
    {       
        if (q_w_proportion_ == nullptr)
            q_w_proportion_ = new std::vector<float>(num_topics_);
        // Compute the proportion
        Row<int64_t>& summary_row = model->GetSummaryRow();
        if (word == -1) // build alias row for beta 
        {
            beta_mass_ = 0;
            for (int32_t k = 0; k < num_topics_; ++k)
            {
                (*q_w_proportion_)[k] = beta_ / (summary_row.At(k) + beta_sum_);
                beta_mass_ += (*q_w_proportion_)[k];
            }
            alias_rng_int_->Build(*q_w_proportion_, num_topics_, beta_mass_, beta_height_, beta_kv_vector_);
        }
        else // build alias row for word
        {            
            WordEntry& word_entry = table_index_->word_entry(word);
            Row<int32_t>& word_topic_row = model->GetWordTopicRow(word);
            int32_t size = 0;
            mass_[word] = 0;
            if (word_entry.is_dense)
            {
                size = num_topics_;
                for (int32_t k = 0; k < num_topics_; ++k)
                {
                    (*q_w_proportion_)[k] = (word_topic_row.At(k) + beta_)
                        / (summary_row.At(k) + beta_sum_);
                    mass_[word] += (*q_w_proportion_)[k];
                }
            }
            else // word_entry.is_dense = false
            {
                word_entry.capacity = word_topic_row.NonzeroSize();
                int32_t* idx_vector = memory_block_ + word_entry.begin_offset 
                    + 2 * word_entry.capacity;
                Row<int32_t>::iterator iter = word_topic_row.Iterator();
                while (iter.HasNext())
                {
                    int32_t t = iter.Key();
                    int32_t n_tw = iter.Value();
                    int64_t n_t = summary_row.At(t);
                    idx_vector[size] = t;
                    (*q_w_proportion_)[size] = (n_tw) / (n_t + beta_sum_);
                    mass_[word] += (*q_w_proportion_)[size];
                    ++size;
                    iter.Next();
                }
                if (size == 0)
                {
                    Log::Error("Fail to build alias row, capacity of row = %d\n",
                        word_topic_row.NonzeroSize());
                }
            }
            alias_rng_int_->Build(*q_w_proportion_, size, mass_[word], height_[word], 
                memory_block_ + word_entry.begin_offset);
        }
        return 0;
    }

    int32_t AliasTable::Propose(int32_t word, xorshift_rng& rng)
    {
        WordEntry& word_entry = table_index_->word_entry(word);
        int32_t* kv_vector = memory_block_ + word_entry.begin_offset;
        int32_t capacity = word_entry.capacity;
        if (word_entry.is_dense)
        {
            return alias_rng_int_->Propose(rng, height_[word], kv_vector);
        }
        else
        {
            return alias_rng_int_->Propose(rng, height_[word], beta_height_,
                mass_[word], beta_mass_, 
                kv_vector, capacity, 
                beta_kv_vector_);
        }
    }

    void AliasTable::Clear()
    {
	delete q_w_proportion_;
	q_w_proportion_ = nullptr;
    }
    // -- AliasTable implement area --------------------------------- //

    // -- AliasMultinomialRNGInt implement area --------------------------------- //
    void AliasMultinomialRNGInt::Build(const std::vector<float>& q_proportion, int32_t size,
        float mass, int32_t & height, int32_t* kv_vector)
    {
        if (q_proportion_int_ == nullptr)
	{
            q_proportion_int_ = new std::vector<int32_t>(size_);
	}
	else if(q_proportion_int_->size() != size_)
	{
            q_proportion_int_->resize(size_); 
	}
        if (L_ == nullptr)
	{
            L_ = new std::vector<std::pair<int32_t, int32_t>>(size_);
	}
	else if(L_->size() != size_)
	{
            L_->resize(size_); 
	}
        if (H_ == nullptr)
	{
            H_ = new std::vector<std::pair<int32_t, int32_t>>(size_);
	}
	else if(H_->size() != size_)
	{
            H_->resize(size_); 
	}

        int32_t mass_int = 0x7fffffff;
        int32_t a_int = mass_int / size;
        mass_int = a_int * size;
        height = a_int;
        int64_t mass_sum = 0;
        for (int32_t i = 0; i < size; ++i)
        {
            (*q_proportion_int_)[i] =
                static_cast<int32_t>(q_proportion[i] / mass * mass_int);
            mass_sum += (*q_proportion_int_)[i];
        }
        if (mass_sum > mass_int)
        {
            int32_t more = static_cast<int32_t>(mass_sum - mass_int);
            int32_t id = 0;
            for (int32_t i = 0; i < more;)
            {
                if ((*q_proportion_int_)[id] >= 1)
                {
                    --(*q_proportion_int_)[id];
                    ++i;
                }
                id = (id + 1) % size;
            }
        }

        if (mass_sum < mass_int)
        {
            int32_t more = static_cast<int32_t>(mass_int - mass_sum);
            int32_t id = 0;
            for (int32_t i = 0; i < more; ++i)
            {
                ++(*q_proportion_int_)[id];
                id = (id + 1) % size;
            }
        }

        for (int32_t k = 0; k < size; ++k)
        {
            int32_t* p = kv_vector + 2 * k;
            *p = k; ++p;
            *p = (k + 1) * height;
        }
        int32_t L_head = 0, L_tail = 0, H_head = 0, H_tail = 0;
        for (int32_t k = 0; k < size; ++k)
        {
            int32_t val = (*q_proportion_int_)[k];
            if (val < height)
            {
                (*L_)[L_tail].first = k;
                (*L_)[L_tail].second = val;
                ++L_tail;
            }
            else
            {
                (*H_)[H_tail].first = k;
                (*H_)[H_tail].second = val;
                ++H_tail;
            }
        }
        while (L_head != L_tail && H_head != H_tail)
        {
            auto& l_pl = (*L_)[L_head++];
            auto& h_ph = (*H_)[H_head++];
            int32_t* p = kv_vector + 2 * l_pl.first;
            *p = h_ph.first; ++p;
            *p = l_pl.first * height + l_pl.second;

            auto sum = h_ph.second + l_pl.second;
            if (sum > 2 * height)
            {
                (*H_)[H_tail].first = h_ph.first;
                (*H_)[H_tail].second = sum - height;
                ++H_tail;
            }
            else
            {
                (*L_)[L_tail].first = h_ph.first;
                (*L_)[L_tail].second = sum - height;
                ++L_tail;
            }
        }
        while (L_head != L_tail)
        {
            auto first = (*L_)[L_head].first;
            auto second = (*L_)[L_head].second;
            int32_t* p = kv_vector + 2 * first;
            *p = first; ++p;
            *p = first * height + second;
            ++L_head;
        }
        while (H_head != H_tail)
        {
            auto first = (*H_)[H_head].first;
            auto second = (*H_)[H_head].second;
            int32_t* p = kv_vector + 2 * first;
            *p = first; ++p;
            *p = first * height + second;
            ++H_head;
        }
    }

    void AliasMultinomialRNGInt::Clear()
    {
	delete q_proportion_int_;
	q_proportion_int_ = nullptr;
	delete L_;
	L_ = nullptr;
	delete H_;
	H_ = nullptr;
    }
    
    int32_t AliasMultinomialRNGInt::Propose(xorshift_rng& rng, int32_t height, 
        int32_t* kv_vector)
    {
        auto sample = rng.rand();
        int32_t idx = sample / height;
        if (size_ <= idx) idx = size_ - 1;
        
        int32_t* p = kv_vector + 2 * idx;
        int32_t k = *p++;
        int32_t v = *p;
        int32_t m = -(sample < v);
        return (idx & m) | (k & ~m);
    }
    
    int32_t AliasMultinomialRNGInt::Propose(xorshift_rng& rng,
        int32_t height, int32_t height_sum, 
        float mass, float mass_sum,
        int32_t* kv_vector, int32_t vsize,
        int32_t* kv_vector_sum)
    {
        auto sample = rng.rand_double() * (mass + mass_sum);
        if (sample < mass)
        {
            int32_t* idx_vector = kv_vector + 2 * vsize;
            auto n_sample = rng.rand();
            int32_t idx = n_sample / height;
            if (vsize <= idx) idx = vsize - 1;
            int32_t* p = kv_vector + 2 * idx;
            int32_t k = *p++;
            int32_t v = *p;
            int32_t id = idx_vector[idx];
            int32_t m = -(n_sample < v);
            return (id & m) | (idx_vector[k] & ~m);
        }
        else
        {
            auto n_sample = rng.rand();
            int32_t idx = n_sample / height_sum;
            if (size_ <= idx) idx = size_ - 1;
            int32_t* p = kv_vector_sum + 2 * idx;
            int32_t k = *p++;
            int32_t v = *p;
            int32_t m = -(n_sample < v);
            return (idx & m) | (k & ~m);
        }
    }
    // -- AliasMultinomialRNGInt implement area --------------------------------- //
} // namespace lightlda
} // namespace multiverso
