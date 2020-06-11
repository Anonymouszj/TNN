// Copyright 2019 Tencent. All Rights Reserved

#include "device/cuda/acc/cuda_conv_3d_layer_acc.h"
#include <iostream>
#include "device/cuda/cuda_utils.h"
#include "utils/dims_vector_utils.h"

namespace TNN_NS {

Status CudaConv3DLayerAcc::Init(Context *context, LayerParam *param,
                                LayerResource *resource,
                                const std::vector<Blob *> &inputs,
                                const std::vector<Blob *> &outputs) {
    CudaLayerAcc::Init(context, param, resource, inputs, outputs);
    alpha_          = 1.0f;
    beta_           = 0.0f;
    workspace_data_ = nullptr;
    workspace_size_ = 0;
    weights_        = nullptr;
    bias_           = nullptr;
    bias_term_      = false;

    DimsVector input_dims  = inputs[0]->GetBlobDesc().dims;
    DimsVector output_dims = outputs[0]->GetBlobDesc().dims;

    Blob *input = inputs[0];

    FetchDimensions(inputs[0], outputs[0], blob_info_);

    ConvLayerParam *conv_param = dynamic_cast<ConvLayerParam *>(param);
    FetchKernelInfo(conv_param, kernel_);

    // LOGD("CudaConv3DLayer param kernel_w: %d, kernel_h: %d \n",
    // conv_param->kernels[0], conv_param->kernels[1]);

    CUDNN_CHECK(cudnnCreateTensorDescriptor(&bottom_desc_));
    CUDNN_CHECK(cudnnCreateTensorDescriptor(&top_desc_));
    CUDNN_CHECK(cudnnCreateFilterDescriptor(&filter_desc_));
    CUDNN_CHECK(cudnnCreateConvolutionDescriptor(&conv_desc_));
    CUDNN_CHECK(cudnnSetConvolutionGroupCount(conv_desc_, kernel_.groups));

    const int filter_dims[] = {
        blob_info_.output_c, blob_info_.input_c / kernel_.groups,
        kernel_.kernel_d, kernel_.kernel_h, kernel_.kernel_w};

    CUDNN_CHECK(cudnnSetFilterNdDescriptor(filter_desc_, CUDNN_DATA_FLOAT,
                                           CUDNN_TENSOR_NCHW, 5, filter_dims));

    const int pad_dims[] = {kernel_.pad_f, kernel_.pad_t,
                            kernel_.pad_l};  // DHW
    const int sti_dims[] = {kernel_.stride_d, kernel_.stride_h,
                            kernel_.stride_w};
    const int dil_dims[] = {kernel_.dilation_d, kernel_.dilation_h,
                            kernel_.dilation_w};

    CUDNN_CHECK(cudnnSetConvolutionNdDescriptor(
        conv_desc_, 3, pad_dims, sti_dims, dil_dims, CUDNN_CROSS_CORRELATION,
        CUDNN_DATA_FLOAT));

    ConvLayerResource *conv_resource =
        dynamic_cast<ConvLayerResource *>(resource);
    float *weights = conv_resource->filter_handle.force_to<float *>();
    // LOGD("weight size: %d \n", conv_resource->filter_handle.GetBytesSize());
    // LOGD("weights0: %f \n", weights[0]);

    size_t weights_size = sizeof(float) * blob_info_.input_c *
                          blob_info_.output_c * kernel_.kernel_d *
                          kernel_.kernel_h * kernel_.kernel_w;

    CUDA_CHECK(cudaMalloc((void **)&weights_, weights_size));
    CUDA_CHECK(
        cudaMemcpy(weights_, weights, weights_size, cudaMemcpyHostToDevice));

    // LOGD("CudaConv3DLayer bias: %d \n", conv_param->bias);
    // LOGD("CudaConv3DLayer bias size: %d \n",
    // conv_resource->bias_handle.GetBytesSize());

    if (conv_param->bias) {
        bias_term_ = true;
        if (blob_info_.output_c * sizeof(float) !=
            conv_resource->bias_handle.GetBytesSize()) {
            return TNNERR_MODEL_ERR;
        }

        const int bias_dim[] = {1, blob_info_.output_c, 1, 1, 1};
        CUDNN_CHECK(cudnnCreateTensorDescriptor(&bias_desc_));
        CUDNN_CHECK(cudnnSetTensorNdDescriptorEx(
            bias_desc_, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT, 5, bias_dim));

        CUDA_CHECK(cudaMalloc((void **)&bias_,
                              conv_resource->bias_handle.GetBytesSize()));
        CUDA_CHECK(cudaMemcpy(
            bias_, conv_resource->bias_handle.force_to<float *>(),
            conv_resource->bias_handle.GetBytesSize(), cudaMemcpyHostToDevice));
    }

    return this->Reshape(inputs, outputs);
}

CudaConv3DLayerAcc::~CudaConv3DLayerAcc() {
    if (workspace_data_ != nullptr) {
        CUDA_CHECK(cudaFree(workspace_data_));
    }
    if (weights_ != nullptr) {
        CUDA_CHECK(cudaFree(weights_));
    }
    if (bias_ != nullptr) {
        CUDA_CHECK(cudaFree(bias_));
    }
    CUDNN_CHECK(cudnnDestroyTensorDescriptor(bottom_desc_));
    CUDNN_CHECK(cudnnDestroyTensorDescriptor(top_desc_));
    CUDNN_CHECK(cudnnDestroyTensorDescriptor(bias_desc_));
    CUDNN_CHECK(cudnnDestroyFilterDescriptor(filter_desc_));
    CUDNN_CHECK(cudnnDestroyConvolutionDescriptor(conv_desc_));
}

Status CudaConv3DLayerAcc::Reshape(const std::vector<Blob *> &inputs,
                                   const std::vector<Blob *> &outputs) {
    DimsVector input_dims  = inputs[0]->GetBlobDesc().dims;
    DimsVector output_dims = outputs[0]->GetBlobDesc().dims;

    Blob *input = inputs[0];

    FetchDimensions(inputs[0], outputs[0], blob_info_);

    // LOGD("input n,c,d,h,w: %d, %d, %d, %d, %d , output n,c,h,w: %d, %d, %d,
    // %d, %d \n",
    //      blob_info_.batch, blob_info_.input_c, blob_info_.input_d,
    //      blob_info_.input_h, blob_info_.input_w,
    //      blob_info_.batch, blob_info_.output_c, blob_info_.output_d,
    //      blob_info_.output_h, blob_info_.output_w);

    int in_dims[] = {blob_info_.batch, blob_info_.input_c, blob_info_.input_d,
                     blob_info_.input_h, blob_info_.input_w};
    CUDNN_CHECK(cudnnSetTensorNdDescriptorEx(bottom_desc_, CUDNN_TENSOR_NCHW,
                                             CUDNN_DATA_FLOAT, 5, in_dims));

    int out_dims[5];
    CUDNN_CHECK(cudnnGetConvolutionNdForwardOutputDim(
        conv_desc_, bottom_desc_, filter_desc_, 5, out_dims));

    // LOGD("conv3d layer acc cudnn infered ncdhw %d %d %d %d %d\n",
    // out_dims[0],
    //      out_dims[1], out_dims[2], out_dims[3], out_dims[4]);

    CUDNN_CHECK(cudnnSetTensorNdDescriptorEx(top_desc_, CUDNN_TENSOR_NCHW,
                                             CUDNN_DATA_FLOAT, 5, out_dims));

    // algorithm
    CUDNN_CHECK(cudnnGetConvolutionForwardAlgorithm(
        context_->cudnn_handle_, bottom_desc_, filter_desc_, conv_desc_,
        top_desc_, CUDNN_CONVOLUTION_FWD_PREFER_FASTEST, 0, &conv_algo_));

    // LOGD("Convolution algorithm: %d\n", conv_algo_);

    // workspace
    size_t needed_workspace_size;
    CUDNN_CHECK(cudnnGetConvolutionForwardWorkspaceSize(
        context_->cudnn_handle_, bottom_desc_, filter_desc_, conv_desc_,
        top_desc_, conv_algo_, &needed_workspace_size));

    // LOGD("Workspace size: %ld\n", workspace_size_);
    if (workspace_size_ < needed_workspace_size) {
        workspace_size_ = needed_workspace_size;
        if (workspace_data_ != nullptr) {
            CUDA_CHECK(cudaFree(workspace_data_));
        }
        CUDA_CHECK(cudaMalloc(&workspace_data_, workspace_size_));
    }

    return TNN_OK;
}

Status CudaConv3DLayerAcc::Forward(const std::vector<Blob *> &inputs,
                                   const std::vector<Blob *> &outputs) {
    CUDNN_CHECK(cudnnConvolutionForward(
        context_->cudnn_handle_, &alpha_, bottom_desc_,
        inputs[0]->GetHandle().base, filter_desc_, weights_, conv_desc_,
        conv_algo_, workspace_data_, workspace_size_, &beta_, top_desc_,
        outputs[0]->GetHandle().base));

    if (bias_term_) {
        float alpha = 1.0f;
        float beta  = 1.0f;
        CUDNN_CHECK(cudnnAddTensor(context_->cudnn_handle_, &alpha, bias_desc_,
                                   bias_, &beta, top_desc_,
                                   outputs[0]->GetHandle().base));
    }

    return TNN_OK;
}

CudaTypeLayerAccRegister<TypeLayerAccCreator<CudaConv3DLayerAcc>>
    g_cuda_conv_3d_layer_acc_register(LAYER_CONVOLUTION_3D);

CudaTypeLayerAccRegister<TypeLayerAccCreator<CudaConv3DLayerAcc>>
    g_cuda_conv_layer_acc_register(LAYER_CONVOLUTION);

}  // namespace TNN_NS
