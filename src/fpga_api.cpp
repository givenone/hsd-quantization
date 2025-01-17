#include "fpga_api.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <cstring>

#define min(x, y) (((x) < (y)) ? (x) : (y))

FPGA::FPGA(off_t data_addr, off_t output_addr, int m_size, int v_size)
{
  m_size_ = m_size;
  v_size_ = v_size;
  data_size_ = (m_size_ + 1) * v_size_ * sizeof(int); // fpga bram data size

  fd_ = open("/dev/mem", O_RDWR);
  qdata_ = static_cast<int *>(mmap(NULL, data_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, data_addr));
  output_ = static_cast<unsigned int *>(mmap(NULL, sizeof(unsigned int), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, output_addr));

  num_block_call_ = 0;
}

FPGA::~FPGA()
{
  munmap(qdata_, data_size_);
  munmap(output_, sizeof(unsigned int));
  close(fd_);
}

int *FPGA::qmatrix(void)
{
  return qdata_ + v_size_;
}

int *FPGA::qvector(void)
{
  return qdata_;
}

void FPGA::reset(void)
{
  num_block_call_ = 0;
}

int FPGA::num_block_call(void)
{
  return num_block_call_;
}

void quantize(const float* input, int* quantized, int num_input, int bits_min, int bits_max, int offset, float scale)
{
  for(int i = 0; i < num_input; i++)
  {
    quantized[i] = (int) ((input[i] / scale)); // TODO: convert floating point to quantized value
  }
}

void dequantize(int* quantized, float* output, int num_output, int offset, float scale)
{
  for(int i = 0; i < num_output; i++)
  {
    output[i] = scale * ((float)quantized[i]); // TODO: convert quantized value to floating point
  }
}

const int *__attribute__((optimize("O0"))) FPGA::qblockMV(Compute* comp)
{
  num_block_call_ += 1;

  // fpga version
  *output_ = 0x5555;
  while (*output_ == 0x5555)
    ;

  return qdata_;
}

void FPGA::largeMV(const float *large_mat, const float *input, float *output, int num_input, int num_output, Compute* comp)
{
  int *vec = this->qvector();
  int *mat = this->qmatrix();

  int *qlarge_mat = new int[num_input*num_output];
  int *qinput = new int[num_input];
  int *qoutput = new int[num_output];
  int *int_output = new int[m_size_];

  // quantize
  int act_bits_min = 0;
  int act_bits_max = (1<<(comp->act_bits-1))-1;

  float act_scale = (comp->act_min - comp->act_max) / act_bits_max; // TODO calculate the scale factor
  int act_offset = (int) ((-1 * comp->act_min) / act_scale); // TODO calculate the zero-offset
  quantize(input, qinput, num_input, act_bits_min, act_bits_max, act_offset, act_scale);

  int weight_bits_min = 0;
  int weight_bits_max = (1<<(comp->weight_bits-1))-1;

    float weight_scale = (comp->weight_max - comp->weight_min) / 127; // TODO calculate the scale factor
    int weight_offset = (int) ((-1 * comp->weight_min)/weight_scale); // TODO calculate the zero-offset
  quantize(large_mat, qlarge_mat, num_input*num_output, weight_bits_min, weight_bits_max, weight_offset, weight_scale);

  // 0) Initialize output vector
  for (int i = 0; i < num_output; ++i)
    qoutput[i] = 0;

  for (int i = 0; i < num_output; i += m_size_)
  {
    for (int j = 0; j < num_input; j += v_size_)
    {
      // 0) Initialize input vector
      int block_row = min(m_size_, num_output - i);
      int block_col = min(v_size_, num_input - j);
      memset(vec, 0, sizeof(int)*v_size_);
      memset(mat, 0, sizeof(int)*m_size_*v_size_);

        memcpy(vec, qinput + j, sizeof(int) * block_col);
        //if(block_col < v_size_) memset()
        // 2) Assign a matrix
        /* IMPLEMENT */
        int k=0;
        for(; k< block_row ; k++)
        {
            memcpy(mat+ v_size_* k, qlarge_mat + (i+k) * num_input + j, sizeof(int) * block_col);
            if(block_col < v_size_) memset(mat+ v_size_ * k + block_col, 0, sizeof(int) * (v_size_ - block_col));
        }
        if(k < m_size_)
        {
            for(int x = 0; x < m_size_ - k ; x++)
            {
                memset(mat+ v_size_ * ( k + x ), 0, sizeof(int) * v_size_);
            }
        }

      // 3) Call a function `qblockMV() to execute MV multiplication
      const int* ret = this->qblockMV(comp);
      
        // for debugging
        for (int i = 0; i < m_size_; ++i)
        {
            int_output[i] = 0;
            for (int j = 0; j < v_size_; ++j)
                int_output[i] += vec[j] * mat[v_size_ * i + j];
        }

      // 4) Accumulate intermediate results
      for(int row = 0; row < block_row; ++row)
        qoutput[i + row] += ret[row];
    }
  }

  dequantize(qoutput, output, num_output, 0, act_scale*weight_scale);
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
