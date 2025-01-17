#include "fpga_api.h"
#include <stdio.h>
#include <iostream>
#include <cstring>

using namespace std;

#define min(x, y) (((x) < (y)) ? (x) : (y))

FPGA::FPGA(off_t data_addr, off_t output_addr, int m_size, int v_size)
{
  m_size_ = m_size;
  v_size_ = v_size;
  data_size_ = (m_size_ + 1) * v_size_; // fpga bram data size

  qvec_ = new char[v_size_];
  qmat_ = new char[m_size_*v_size_];
  qout_ = new short[m_size_];

  output_ = new unsigned int[m_size_]; // use output_ as tempolar output
  data_ = new float[data_size_];

  num_block_call_ = 0;
}

FPGA::~FPGA()
{
  delete[] output_;
  delete[] data_;
  delete[] qvec_;
  delete[] qmat_;
  delete[] qout_;
}

float *FPGA::matrix(void)
{
  return data_ + v_size_;
}

float *FPGA::vector(void)
{
  return data_;
}

void FPGA::reset(void)
{
  num_block_call_ = 0;
}

int FPGA::num_block_call(void)
{
  return num_block_call_;
}

void quantize(float* input, char* quantized, int num_input, char bits_min, char bits_max, char offset, float scale)
{
  for(int i = 0; i < num_input; i++)
  {
    quantized[i] = (char) ((input[i] / scale)); // TODO: convert floating point to quantized value
  }
}

void dequantize(short* quantized, float* output, int num_output, char offset, float scale)
{
  for(int i = 0; i < num_output; i++)
  {
    output[i] = scale * ((float)quantized[i]); // TODO: convert quantized value to floating point
    //printf("%f ", output[i]);
  }
}

const float *FPGA::blockMV(Compute* comp)
{
  num_block_call_ += 1;

  // cpu version
  float *vec = this->vector();
  float *mat = this->matrix();
  float *out = reinterpret_cast<float *>(output_);

  if(comp->quantized)
  {
    char act_bits_min = 0;
    char act_bits_max = (1<<(comp->act_bits-1))-1;

    float act_scale = (comp->act_min - comp->act_max) / act_bits_max; // TODO calculate the scale factor
    char act_offset = (char) (-1 * comp->act_min) / act_scale; // TODO calculate the zero-offset
    quantize(vec, qvec_, v_size_, act_bits_min, act_bits_max, act_offset, act_scale);

    char weight_bits_min = 0;
    char weight_bits_max = (1<<(comp->weight_bits-1))-1;

    float weight_scale = (comp->weight_max - comp->weight_min) / 127; // TODO calculate the scale factor
    char weight_offset = (char) (-1 * comp->weight_min) / weight_scale; // TODO calculate the zero-offset
    quantize(mat, qmat_, m_size_*v_size_, weight_bits_min, weight_bits_max, weight_offset, weight_scale);

    //printf("%d %d, %f %d", act_bits_min, act_bits_max, act_scale, act_offset);
    for (int i = 0; i < m_size_; ++i)
    {
      qout_[i] = 0;
      for (int j = 0; j < v_size_; ++j)
        qout_[i] += (qvec_[j]) * (qmat_[v_size_ * i + j]);
      //printf("%d ", qout_[i]);
    }

    dequantize(qout_, out, m_size_, 0, act_scale*weight_scale);
  }
  else
  {
    for (int i = 0; i < m_size_; ++i)
    {
      out[i] = 0;
      for (int j = 0; j < v_size_; ++j)
        out[i] += vec[j] * mat[v_size_ * i + j];
    }
  }

  for (int i = 0; i < m_size_; ++i)
    data_[i] = out[i];

  return data_;
}

void FPGA::largeMV(const float *large_mat, const float *input, float *output, int num_input, int num_output, Compute* comp)
{
  float *vec = this->vector();
  float *mat = this->matrix();

  // 0) Initialize output vector
  for (int i = 0; i < num_output; ++i)
    output[i] = 0;

  for (int i = 0; i < num_output; i += m_size_)
  {
    for (int j = 0; j < num_input; j += v_size_)
    {
        // 0) Initialize input vector		
        int block_row = min(m_size_, num_output-i);
        int block_col = min(v_size_, num_input-j);

        // !) Assign a vector
        /* IMPLEMENT */
        memcpy(vec, input + j, sizeof(float) * block_col);
        //if(block_col < v_size_) memset()
        // 2) Assign a matrix
        /* IMPLEMENT */
        int k=0;
        for(; k< block_row ; k++)
        {
            memcpy(mat+ v_size_* k, large_mat + (i+k) * num_input + j, sizeof(float) * block_col);
            if(block_col < v_size_) memset(mat+ v_size_ * k + block_col, 0, sizeof(float) * (v_size_ - block_col));
        }
        if(k < m_size_)
        {
            for(int x = 0; x < m_size_ - k ; x++)
            {
                memset(mat+ v_size_ * ( k + x ), 0, sizeof(float) * v_size_);
            }
        }
        // 3) Call a function `block_call() to execute MV multiplication
        const float* ret = this->blockMV(comp);

        // 4) Accumulate intermediate results
        for(int row = 0; row < block_row; ++row)
        {
            output[i + row] += ret[row];
        }
    }
  }
}

void FPGA::convLowering(const std::vector<std::vector<std::vector<std::vector<float>>>> &cnn_weights,
                        std::vector<std::vector<float>> &new_weights,
                        const std::vector<std::vector<std::vector<float>>> &inputs,
                        std::vector<std::vector<float>> &new_inputs)
{
  /*
   * Arguments:
   *
   * conv_weights: [conv_channel, input_channel, conv_height, conv_width]
   * new_weights: [?, ?]
   * inputs: [input_channel, input_height, input_width]
   * new_inputs: [?, ?]
   *
   */

  int conv_channel = cnn_weights.size();
  int input_channel = cnn_weights[0].size();
  int conv_height = cnn_weights[0][0].size();
  int conv_width = cnn_weights[0][0][0].size();
  //int input_channel = inputs.size();
  int input_height = inputs[0].size();
  int input_width = inputs[0][0].size();
  // IMPLEMENT THIS
  // For example,
  // new_weights[0][0] = cnn_weights[0][0][0][0];
  // new_inputs[0][0] = inputs[0][0][0];
    for(int c=0; c<conv_channel; c++)
    {
      // vector row new_weight[c] 
      int cnt = 0;
      for(int ic = 0; ic<input_channel; ic++)
      {
          for(int h=0; h<conv_height; h++)
          {
              for(int w=0; w<conv_width; w++)
              {
                  new_weights[c][cnt++] = (cnn_weights[c][ic][h][w]);
              }
          }
      }
    }

  // first move row-wise  
  int cnt = 0;
  for(int y=0; y<input_height-conv_height+1; y++)
  {
      for(int x=0; x<input_width-conv_width+1; x++)
      {
          // input row new_input[cnt];
          for(int ic=0; ic<input_channel; ic++)
          {
                for(int h=0; h<conv_height; h++)
                {
                    for(int w=0; w<conv_width; w++)
                    {
                        int idx = w + conv_width * h + (conv_width*conv_height) * ic;
                        new_inputs[idx][cnt] = inputs[ic][h+y][w+x]; 
                    }
                }
          }
        cnt++;
      }

  }
}
